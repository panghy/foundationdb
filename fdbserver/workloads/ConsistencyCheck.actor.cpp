/*
 * ConsistencyCheck.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "flow/IRandom.h"
#include "fdbclient/NativeAPI.h"
#include "fdbserver/TesterInterface.h"
#include "workloads.h"
#include "fdbrpc/IRateControl.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/StorageMetrics.h"
#include "fdbserver/DataDistribution.h"
#include "fdbserver/QuietDatabase.h"
#include "flow/DeterministicRandom.h"
#include "fdbclient/ManagementAPI.h"

struct ConsistencyCheckWorkload : TestWorkload
{
	//Whether or not we should perform checks that will only pass if the database is in a quiescent state
	bool performQuiescentChecks;

	//How long to wait for the database to go quiet before failing (if doing quiescent checks)
	double quiescentWaitTimeout;

	//If true, then perform all checks on this client.  The first client is the only one to perform all of the fast checks
	//All other clients will perform slow checks if this test is distributed
	bool firstClient;

	//If true, then the expensive checks will be distributed to multiple clients
	bool distributed;

	//Determines how many shards are checked for consistency: out of every <shardSampleFactor> shards, 1 will be checked
	int shardSampleFactor;

	//The previous data distribution mode
	int oldDataDistributionMode;

	//If true, then any failure of the consistency check will be logged as SevError.  Otherwise, it will be logged as SevWarn
	bool failureIsError;

	//Ideal number of bytes per second to read from each storage server
	int rateLimit;

	//Randomize shard order with each iteration if true
	bool shuffleShards;

	bool success;

	//Number of times this client has run its portion of the consistency check
	int64_t repetitions;

	//Whether to continuously perfom the consistency check
	bool indefinite;

	ConsistencyCheckWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		performQuiescentChecks = getOption(options, LiteralStringRef("performQuiescentChecks"), false);
		quiescentWaitTimeout = getOption(options, LiteralStringRef("quiescentWaitTimeout"), 600.0);
		distributed = getOption(options, LiteralStringRef("distributed"), true);
		shardSampleFactor = std::max(getOption(options, LiteralStringRef("shardSampleFactor"), 1), 1);
		failureIsError = getOption(options, LiteralStringRef("failureIsError"), false);
		rateLimit = getOption(options, LiteralStringRef("rateLimit"), 0);
		shuffleShards = getOption(options, LiteralStringRef("shuffleShards"), false);
		indefinite = getOption(options, LiteralStringRef("indefinite"), false);

		success = true;

		firstClient = clientId == 0;

		repetitions = 0;
	}

	virtual std::string description()
	{
		return "ConsistencyCheck";
	}

	virtual Future<Void> setup(Database const& cx)
	{
		return _setup(cx, this);
	}

	ACTOR Future<Void> _setup(Database cx, ConsistencyCheckWorkload *self)
	{
		//If performing quiescent checks, wait for the database to go quiet
		if(self->firstClient && self->performQuiescentChecks)
		{
			if(g_network->isSimulated()) {
				Void _ = wait( timeKeeperSetDisable(cx) );
			}

			try
			{
				Void _ = wait(timeoutError(quietDatabase(cx, self->dbInfo, "ConsistencyCheckStart", 0, 1e5, 0, 0), self->quiescentWaitTimeout));  // FIXME: should be zero?
			}
			catch(Error& e)
			{
				TraceEvent("ConsistencyCheck_QuietDatabaseError").error(e);
				self->testFailure("Unable to achieve a quiet database");
				self->performQuiescentChecks = false;
			}
		}

		return Void();
	}

	virtual Future<Void> start(Database const& cx)
	{
		TraceEvent("ConsistencyCheck");
		return _start(cx, this);
	}

	virtual Future<bool> check(Database const& cx)
	{
		return success;
	}

	virtual void getMetrics( vector<PerfMetric>& m )
	{

	}

	void testFailure(std::string message)
	{
		success = false;

		TraceEvent failEvent(failureIsError ? SevError : SevWarn, "TestFailure");
		if(performQuiescentChecks)
			failEvent.detail("Workload", "QuiescentCheck");
		else
			failEvent.detail("Workload", "ConsistencyCheck");

		failEvent.detail("Reason", "Consistency check: " + message);
	}

	ACTOR Future<Void> _start(Database cx, ConsistencyCheckWorkload *self)
	{
		loop {
			Void _ = wait(self->runCheck(cx, self));
			if(!self->indefinite)
				break;
			self->repetitions++;
			Void _ = wait(delay(5.0));
		}
		return Void();
	}

	ACTOR Future<Void> runCheck(Database cx, ConsistencyCheckWorkload *self)
	{
		TEST(self->performQuiescentChecks); //Quiescent consistency check
		TEST(!self->performQuiescentChecks); //Non-quiescent consistency check

		if(self->firstClient || self->distributed)
		{
			try
			{
				state DatabaseConfiguration configuration;

				state Transaction tr(cx);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				loop {
					try {
						Standalone<RangeResultRef> res = wait( tr.getRange(configKeys, 1000) );
						if( res.size() == 1000 ) {
							TraceEvent("ConsistencyCheck_TooManyConfigOptions");
							self->testFailure("Read too many configuration options");
						}
						for( int i = 0; i < res.size(); i++ )
							configuration.set(res[i].key,res[i].value);
						break;
					} catch( Error &e ) {
						Void _ = wait( tr.onError(e) );
					}
				}

				//Perform quiescence-only checks
				if(self->firstClient && self->performQuiescentChecks)
				{
					//Check for undesirable servers (storage servers with exact same network address or using the wrong key value store type)
					state bool hasUndesirableServers = wait(self->checkForUndesirableServers(cx, configuration, self));

					//Check that nothing is in-flight or in queue in data distribution
					int64_t inDataDistributionQueue = wait(getDataDistributionQueueSize(cx, self->dbInfo, true));
					if(inDataDistributionQueue > 0)
					{
						TraceEvent("ConsistencyCheck_NonZeroDataDistributionQueue").detail("QueueSize", inDataDistributionQueue);
						self->testFailure("Non-zero data distribution queue/in-flight size");
					}

					//Check that nothing is in the TLog queues
					int64_t maxTLogQueueSize = wait(getMaxTLogQueueSize(cx, self->dbInfo));
					if(maxTLogQueueSize > 1e5)  // FIXME: Should be zero?
					{
						TraceEvent("ConsistencyCheck_NonZeroTLogQueue").detail("MaxQueueSize", maxTLogQueueSize);
						self->testFailure("Non-zero tlog queue size");
					}

					//Check that nothing is in the storage server queues
					try
					{
						int64_t maxStorageServerQueueSize = wait(getMaxStorageServerQueueSize(cx, self->dbInfo));
						if(maxStorageServerQueueSize > 0)
						{
							TraceEvent("ConsistencyCheck_NonZeroStorageServerQueue").detail("MaxQueueSize", maxStorageServerQueueSize);
							self->testFailure("Non-zero storage server queue size");
						}
					}
					catch(Error& e)
					{
						if(e.code() == error_code_attribute_not_found)
						{
							TraceEvent("ConsistencyCheck_StorageQueueSizeError").detail("Reason", "Could not read queue size").error(e);

							//This error occurs if we have undesirable servers; in that case just report the undesirable servers error
							if(!hasUndesirableServers)
								self->testFailure("Could not read storage queue size");
						}
						else
							throw;
					}

					bool hasStorage = wait( self->checkForStorage(cx, configuration, self) );
					bool hasExtraStores = wait( self->checkForExtraDataStores(cx, self) );

					//Check that each machine is operating as its desired class
					bool usingDesiredClasses = wait(self->checkUsingDesiredClasses(cx, self));
					if(!usingDesiredClasses)
						self->testFailure("Cluster has machine(s) not using requested classes");

					bool workerListCorrect = wait( self->checkWorkerList(cx, self) );
					if(!workerListCorrect)
						self->testFailure("Worker list incorrect");
				}

				//Get a list of key servers; verify that the TLogs and master all agree about who the key servers are
				state Promise<vector<pair<KeyRange, vector<StorageServerInterface>>>> keyServerPromise;
				bool keyServerResult = wait(self->getKeyServers(cx, self, keyServerPromise));
				if(keyServerResult)
				{
					state vector<pair<KeyRange, vector<StorageServerInterface>>> keyServers = keyServerPromise.getFuture().get();

					//Get the locations of all the shards in the database
					state Promise<Standalone<VectorRef<KeyValueRef>>> keyLocationPromise;
					bool keyLocationResult = wait(self->getKeyLocations(cx, keyServers, self, keyLocationPromise));
					if(keyLocationResult)
					{
						state Standalone<VectorRef<KeyValueRef>> keyLocations = keyLocationPromise.getFuture().get();

						//Check that each shard has the same data on all storage servers that it resides on
						bool dataConsistencyResult = wait(self->checkDataConsistency(cx, keyLocations, configuration, self));
					}
				}
			}
			catch(Error &e)
			{
				if(e.code() == error_code_transaction_too_old || e.code() == error_code_future_version || e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed || e.code() == error_code_server_request_queue_full)
					TraceEvent("ConsistencyCheck_Retry").error(e); // FIXME: consistency check does not retry in this case
				else
					self->testFailure(format("Error %d - %s", e.code(), e.name()));
			}
		}

		TraceEvent("ConsistencyCheck_FinishedCheck").detail("repetitions", self->repetitions);

		return Void();
	}

	//Gets a version at which to read from the storage servers
	ACTOR Future<Version> getVersion(Database cx, ConsistencyCheckWorkload *self)
	{
		loop
		{
			state Transaction tr(cx);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			try
			{
				Version version = wait(tr.getReadVersion());
				return version;
			}
			catch(Error &e)
			{
				tr.onError(e);
			}
		}
	}

	//Get a list of storage servers from the master and compares them with the TLogs.
	//If this is a quiescent check, then each master proxy needs to respond, otherwise only one needs to respond.
	//Returns false if there is a failure (in this case, keyServersPromise will never be set)
	ACTOR Future<bool> getKeyServers(Database cx, ConsistencyCheckWorkload *self, Promise<vector<pair<KeyRange, vector<StorageServerInterface>>>> keyServersPromise)
	{
		state vector<pair<KeyRange, vector<StorageServerInterface>>> keyServers;

		//Try getting key server locations from the master proxies
		state vector<Future<ErrorOr<GetKeyServerLocationsReply>>> keyServerLocationFutures;
		state Key begin = keyServersKeys.begin;
		state Key end = keyServersKeys.end;
		state int limitKeyServers = BUGGIFY ? 1 : 100;

		while (begin < end) {
			state Reference<ProxyInfo> proxyInfo = wait(cx->getMasterProxiesFuture());
			keyServerLocationFutures.clear();
			for (int i = 0; i < proxyInfo->size(); i++)
				keyServerLocationFutures.push_back(proxyInfo->get(i, &MasterProxyInterface::getKeyServersLocations).getReplyUnlessFailedFor(GetKeyServerLocationsRequest(begin, end, limitKeyServers, false, Arena()), 2, 0));

			state bool keyServersInsertedForThisIteration = false;
			choose {
				when(Void _ = wait(waitForAll(keyServerLocationFutures))) {
					//Read the key server location results
					for (int i = 0; i < keyServerLocationFutures.size(); i++)
					{
						ErrorOr<GetKeyServerLocationsReply> shards = keyServerLocationFutures[i].get();

						//If performing quiescent check, then all master proxies should be reachable.  Otherwise, only one needs to be reachable
						if (self->performQuiescentChecks && !shards.present())
						{
							TraceEvent("ConsistencyCheck_MasterProxyUnavailable").detail("MasterProxyID", proxyInfo->getId(i));
							self->testFailure("Master proxy unavailable");
							return false;
						}

						//Get the list of shards if one was returned.  If not doing a quiescent check, we can break if it is.
						//If we are doing a quiescent check, then we only need to do this for the first shard.
						if (shards.present() && !keyServersInsertedForThisIteration)
						{
							keyServers.insert(keyServers.end(), shards.get().results.begin(), shards.get().results.end());
							keyServersInsertedForThisIteration = true;
							begin = shards.get().results.back().first.end;

							if (!self->performQuiescentChecks)
								break;
						}
					} // End of For	
				}
				when(Void _ = wait(cx->onMasterProxiesChanged())) { }
			} // End of choose		
			
			if (!keyServersInsertedForThisIteration) // Retry the entire workflow
				Void _ = wait(delay(1.0));

		} // End of while

		keyServersPromise.send(keyServers);
		return true;
	}

	//Retrieves the locations of all shards in the database
	//Returns false if there is a failure (in this case, keyLocationPromise will never be set)
	ACTOR Future<bool> getKeyLocations(Database cx, vector<pair<KeyRange, vector<StorageServerInterface>>> shards, ConsistencyCheckWorkload *self, Promise<Standalone<VectorRef<KeyValueRef>>> keyLocationPromise)
	{
		state Standalone<VectorRef<KeyValueRef>> keyLocations;
		state Key beginKey = allKeys.begin.withPrefix(keyServersPrefix);
		state Key endKey = allKeys.end.withPrefix(keyServersPrefix);
		state int i = 0;

		//If the responses are too big, we may use multiple requests to get the key locations.  Each request begins where the last left off
		for ( ; i < shards.size(); i++)
		{
			while(beginKey < std::min<KeyRef>(shards[i].first.end, endKey))
			{
				try
				{
					Version version = wait(self->getVersion(cx, self));

					GetKeyValuesRequest req;
					req.begin = firstGreaterOrEqual(beginKey);
					req.end = firstGreaterOrEqual(std::min<KeyRef>(shards[i].first.end, endKey));
					req.limit = SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT;
					req.limitBytes = SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT_BYTES;
					req.version = version;

					//Try getting the shard locations from the key servers
					state vector<Future<ErrorOr<GetKeyValuesReply>>> keyValueFutures;
					for(int j = 0; j < shards[i].second.size(); j++)
					{
						resetReply(req);
						keyValueFutures.push_back(shards[i].second[j].getKeyValues.getReplyUnlessFailedFor(req, 2, 0));
					}

					Void _ = wait(waitForAll(keyValueFutures));

					int firstValidStorageServer = -1;

					//Read the shard location results
					for(int j = 0; j < keyValueFutures.size(); j++)
					{
						ErrorOr<GetKeyValuesReply> reply = keyValueFutures[j].get();

						if(!reply.present())
						{
							//If the storage server didn't reply in a quiescent database, then the check fails
							if(self->performQuiescentChecks)
							{
								TraceEvent("ConsistencyCheck_KeyServerUnavailable").detail("StorageServer", shards[i].second[j].id().toString().c_str());
								self->testFailure("Key server unavailable");
								return false;
							}

							//If no storage servers replied, then throw all_alternatives_failed to force a retry
							else if(firstValidStorageServer < 0 && j == keyValueFutures.size() - 1)
								throw all_alternatives_failed();
						}

						//If this is the first storage server, store the locations to send back to the caller
						else if(firstValidStorageServer < 0)
							firstValidStorageServer = j;

						//Otherwise, compare the data to the results from the first storage server.  If they are different, then the check fails
						else if(reply.get().data != keyValueFutures[firstValidStorageServer].get().get().data || reply.get().more != keyValueFutures[firstValidStorageServer].get().get().more)
						{
							TraceEvent("ConsistencyCheck_InconsistentKeyServers").detail("StorageServer1", shards[i].second[firstValidStorageServer].id())
								.detail("StorageServer2", shards[i].second[j].id());
							self->testFailure("Key servers inconsistent");
							return false;
						}
					}

					auto keyValueResponse = keyValueFutures[firstValidStorageServer].get().get();
					Standalone<RangeResultRef> currentLocations = krmDecodeRanges( keyServersPrefix, KeyRangeRef(beginKey.removePrefix(keyServersPrefix), std::min<KeyRef>(shards[i].first.end, endKey).removePrefix(keyServersPrefix)), RangeResultRef( keyValueResponse.data, keyValueResponse.more) );

					if(keyValueResponse.data.size() && beginKey == keyValueResponse.data[0].key) {
						keyLocations.push_back_deep(keyLocations.arena(), currentLocations[0]);
					}

					if(currentLocations.size() > 2) {
						keyLocations.append_deep(keyLocations.arena(), &currentLocations[1], currentLocations.size() - 2);
					}

					//Next iteration should pick up where we left off
					ASSERT(currentLocations.size() > 1);
					if(!keyValueResponse.more) {
						beginKey = shards[i].first.end;
					} else {
						beginKey = keyValueResponse.data.end()[-1].key;
					}

					//If this is the last iteration, then push the allKeys.end KV pair
					if(beginKey >= endKey)
						keyLocations.push_back_deep(keyLocations.arena(), currentLocations.end()[-1]);
				}
				catch(Error &e)
				{
					//If we failed because of a version problem, then retry
					if(e.code() == error_code_transaction_too_old || e.code() == error_code_future_version || e.code() == error_code_transaction_too_old)
						TraceEvent("ConsistencyCheck_RetryGetKeyLocations").error(e);
					else
						throw;
				}
			}
		}


		keyLocationPromise.send(keyLocations);
		return true;
	}

	//Retrieves a vector of the storage servers' estimates for the size of a particular shard
	//If a storage server can't be reached, its estimate will be -1
	//If there is an error, then the returned vector will have 0 size
	ACTOR Future<vector<int64_t>> getStorageSizeEstimate(vector<StorageServerInterface> storageServers, KeyRangeRef shard)
	{
		state vector<int64_t> estimatedBytes;

		state WaitMetricsRequest req;
		req.keys = shard;
		req.max.bytes = -1;
		req.min.bytes = 0;

		state vector<Future<ErrorOr<StorageMetrics>>> metricFutures;

		try
		{
			//Check the size of the shard on each storage server
			for(int i = 0; i < storageServers.size(); i++)
			{
				resetReply(req);
				metricFutures.push_back(storageServers[i].waitMetrics.getReplyUnlessFailedFor(req, 2, 0));
			}

			//Wait for the storage servers to respond
			Void _ = wait(waitForAll(metricFutures));

			int firstValidStorageServer = -1;

			//Retrieve the size from the storage server responses
			for(int i = 0; i < storageServers.size(); i++)
			{
				ErrorOr<StorageMetrics> reply = metricFutures[i].get();

				//If the storage server doesn't reply, then return -1
				if(!reply.present())
				{
					TraceEvent("ConsistencyCheck_FailedToFetchMetrics").detail("Begin", printable(shard.begin)).detail("End", printable(shard.end)).detail("StorageServer", storageServers[i].id());
					estimatedBytes.push_back(-1);
				}

				//Add the result to the list of estimates
				else if(reply.present())
				{
					int64_t numBytes = reply.get().bytes;
					estimatedBytes.push_back(numBytes);
					if(firstValidStorageServer < 0)
						firstValidStorageServer = i;
					else if(estimatedBytes[firstValidStorageServer] != numBytes)
					{
						TraceEvent("ConsistencyCheck_InconsistentStorageMetrics").detail("ByteEstimate1", estimatedBytes[firstValidStorageServer]).detail("ByteEstimate2", numBytes)
							.detail("Begin", printable(shard.begin)).detail("End", printable(shard.end)).detail("StorageServer1", storageServers[firstValidStorageServer].id())
							.detail("StorageServer2", storageServers[i].id());
					}
				}
			}
		}
		catch(Error& e)
		{
			TraceEvent("ConsistencyCheck_ErrorFetchingMetrics").detail("Begin", printable(shard.begin)).detail("End", printable(shard.end)).error(e);
			estimatedBytes.clear();
		}

		return estimatedBytes;
	}

	//Comparison function used to compare map elements by value
	template<class K, class T>
	static bool compareByValue(std::pair<K, T> a, std::pair<K, T> b)
	{
		return a.second < b.second;
	}

	ACTOR Future<int64_t> getDatabaseSize(Database cx) {
		state Transaction tr( cx );
		tr.setOption(FDBTransactionOptions::LOCK_AWARE);
		loop {
			try {
				StorageMetrics metrics = wait( tr.getStorageMetrics( KeyRangeRef(allKeys.begin, keyServersPrefix), 100000 ) );
				return metrics.bytes;
			} catch( Error &e ) {
				Void _ = wait( tr.onError( e ) );
			}
		}
	}

	//Checks that the data in each shard is the same on each storage server that it resides on.  Also performs some sanity checks on the sizes of shards and storage servers.
	//Returns false if there is a failure
	ACTOR Future<bool> checkDataConsistency(Database cx, VectorRef<KeyValueRef> keyLocations, DatabaseConfiguration configuration, ConsistencyCheckWorkload *self)
	{
		//Stores the total number of bytes on each storage server
		//In a distributed test, this will be an estimated size
		state std::map<UID, int64_t> storageServerSizes;

		//Iterate through each shard, checking its values on all of its storage servers
		//If shardSampleFactor > 1, then not all shards are processed
		//Also, in a distributed data consistency check, each client processes a subset of the shards
		//Note: this may cause some shards to be processed more than once or not at all in a non-quiescent database
		state int effectiveClientCount = (self->distributed) ? self->clientCount : 1;
		state int i = self->clientId * (self->shardSampleFactor + 1);
		state int increment = (self->distributed && !self->firstClient) ? effectiveClientCount * self->shardSampleFactor : 1;
		state Reference<IRateControl> rateLimiter = Reference<IRateControl>( new SpeedLimit(self->rateLimit, CLIENT_KNOBS->CONSISTENCY_CHECK_RATE_WINDOW) );

		state double dbSize = 100e12;
		if(g_network->isSimulated()) {
			//This call will get all shard ranges in the database, which is too expensive on real clusters.
			int64_t _dbSize = wait( self->getDatabaseSize( cx ) );
			dbSize = _dbSize;
		}

		state vector<KeyRangeRef> ranges;

		for(int k = 0; k < keyLocations.size() - 1; k++)
		{
			KeyRangeRef range(keyLocations[k].key, keyLocations[k + 1].key);
			ranges.push_back(range);
		}

		state vector<int> shardOrder;
		for(int k = 0; k < ranges.size(); k++)
			shardOrder.push_back(k);
		if(self->shuffleShards) {
			uint32_t seed = self->sharedRandomNumber + self->repetitions;
			DeterministicRandom sharedRandom( seed == 0 ? 1 : seed );
			sharedRandom.randomShuffle(shardOrder);
		}

		for(; i < ranges.size(); i += increment)
		{
			state int shard = shardOrder[i];

			state KeyRangeRef range = ranges[shard];
			state vector<UID> sourceStorageServers;
			state vector<UID> destStorageServers;
			state Transaction tr(cx);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			state int bytesReadInRange = 0;

			decodeKeyServersValue(keyLocations[shard].value, sourceStorageServers, destStorageServers);

			//If the destStorageServers is non-empty, then this shard is being relocated
			state bool isRelocating = destStorageServers.size() > 0;

			//This check was disabled because we now disable data distribution during the consistency check,
			//which can leave shards with dest storage servers.

			//Disallow relocations in a quiescent database
			/*if(self->firstClient && self->performQuiescentChecks && isRelocating)
			{
				TraceEvent("ConsistencyCheck_QuiescentShardRelocation").detail("ShardBegin", printable(range.start)).detail("ShardEnd", printable(range.end));
				self->testFailure("Shard is being relocated in quiescent database");
				return false;
			}*/

			//In a quiescent database, check that the team size is the same as the desired team size
			if(self->firstClient && self->performQuiescentChecks && sourceStorageServers.size() != configuration.storageTeamSize)
			{
				TraceEvent("ConsistencyCheck_InvalidTeamSize").detail("ShardBegin", printable(range.begin)).detail("ShardEnd", printable(range.end)).detail("teamSize", sourceStorageServers.size()).detail("desiredTeamSize", configuration.storageTeamSize);
				self->testFailure("Invalid team size");
				return false;
			}

			state vector<UID> storageServers = (isRelocating) ? destStorageServers : sourceStorageServers;
			state vector<StorageServerInterface> storageServerInterfaces;

			loop {
				try {
					vector< Future< Optional<Value> > > serverListEntries;
					for(int s=0; s<storageServers.size(); s++)
						serverListEntries.push_back( tr.get( serverListKeyFor(storageServers[s]) ) );
					state vector<Optional<Value>> serverListValues = wait( getAll(serverListEntries) );
					for(int s=0; s<serverListValues.size(); s++) {
						if (serverListValues[s].present())
							storageServerInterfaces.push_back( decodeServerListValue(serverListValues[s].get()) );
						else if (self->performQuiescentChecks)
							self->testFailure("/FF/serverList changing in a quiescent database");
					}
					break;
				}
				catch(Error &e) {
					Void _ = wait( tr.onError(e) );
				}
			}

			state vector<int64_t> estimatedBytes = wait(self->getStorageSizeEstimate(storageServerInterfaces, range));

			//Gets permitted size range of shard
			int64_t maxShardSize = getMaxShardSize( dbSize );
			state ShardSizeBounds shardBounds = getShardSizeBounds(range, maxShardSize);

			if(self->firstClient)
			{
				//If there was an error retrieving shard estimated size
				if(self->performQuiescentChecks && estimatedBytes.size() == 0)
					self->testFailure("Error fetching storage metrics");

				//If running a distributed test, storage server size is an accumulation of shard estimates
				else if(self->distributed && self->firstClient)
					for(int j = 0; j < storageServers.size(); j++)
						storageServerSizes[storageServers[j]] += std::max(estimatedBytes[j], (int64_t)0);
			}

			//The first client may need to skip the rest of the loop contents if it is just processing this shard to get a size estimate
			if(!self->firstClient || shard % (effectiveClientCount * self->shardSampleFactor) == 0)
			{
				state int shardKeys = 0;
				state int shardBytes = 0;
				state int sampledBytes = 0;
				state int splitBytes = 0;
				state int firstKeySampledBytes = 0;
				state int sampledKeys = 0;
				state double shardVariance = 0;
				state bool canSplit = false;
				state Key lastSampleKey;
				state Key lastStartSampleKey;
				state int64_t totalReadAmount = 0;

				state KeySelector begin = firstGreaterOrEqual(range.begin);

				//Read a limited number of entries at a time, repeating until all keys in the shard have been read
				loop
				{
					try
					{
						lastSampleKey = lastStartSampleKey;

						//Get the min version of the storage servers
						Version version = wait(self->getVersion(cx, self));

						state GetKeyValuesRequest req;
						req.begin = begin;
						req.end = firstGreaterOrEqual(range.end);
						req.limit = 1e4;
						req.limitBytes = CLIENT_KNOBS->REPLY_BYTE_LIMIT;
						req.version = version;

						//Try getting the entries in the specified range
						state vector<Future<ErrorOr<GetKeyValuesReply>>> keyValueFutures;
						state int j = 0;
						for(j = 0; j < storageServerInterfaces.size(); j++)
						{
							resetReply(req);
							keyValueFutures.push_back(storageServerInterfaces[j].getKeyValues.getReplyUnlessFailedFor(req, 2, 0));
						}

						Void _ = wait(waitForAll(keyValueFutures));

						//Read the resulting entries
						state int firstValidServer = -1;
						totalReadAmount = 0;
						for(j = 0 ; j < keyValueFutures.size(); j++)
						{
							ErrorOr<GetKeyValuesReply> rangeResult = keyValueFutures[j].get();

							//Compare the results with other storage servers
							if(rangeResult.present())
							{
								state GetKeyValuesReply current = rangeResult.get();
								totalReadAmount += current.data.expectedSize();
								//If we haven't encountered a valid storage server yet, then mark this as the baseline to compare against
								if(firstValidServer == -1)
									firstValidServer = j;

								//Compare this shard against the first
								else
								{
									GetKeyValuesReply reference = keyValueFutures[firstValidServer].get().get();

									if(current.data != reference.data || current.more != reference.more)
									{
										//Be especially verbose if in simulation
										if(g_network->isSimulated())
										{
											int invalidIndex = -1;
											printf("\nSERVER %d (%s); shard = %s - %s:\n", j, storageServerInterfaces[j].address().toString().c_str(), printable(req.begin.getKey()).c_str(), printable(req.end.getKey()).c_str());
											for(int k = 0; k < current.data.size(); k++)
											{
												printf("%d. %s => %s\n", k, printable(current.data[k].key).c_str(), printable(current.data[k].value).c_str());
												if(invalidIndex < 0 && (k >= reference.data.size() || current.data[k].key != reference.data[k].key || current.data[k].value != reference.data[k].value))
													invalidIndex = k;
											}

											printf("\nSERVER %d (%s); shard = %s - %s:\n", firstValidServer, storageServerInterfaces[firstValidServer].address().toString().c_str(), printable(req.begin.getKey()).c_str(), printable(req.end.getKey()).c_str());
											for(int k = 0; k < reference.data.size(); k++)
											{
												printf("%d. %s => %s\n", k, printable(reference.data[k].key).c_str(), printable(reference.data[k].value).c_str());
												if(invalidIndex < 0 && (k >= current.data.size() || reference.data[k].key != current.data[k].key || reference.data[k].value != current.data[k].value))
													invalidIndex = k;
											}

											printf("\nMISMATCH AT %d\n\n", invalidIndex);
										}

										//Data for trace event
										//The number of keys unique to the current shard
										int currentUniques = 0;
										//The number of keys unique to the reference shard
										int referenceUniques = 0;
										//The number of keys in both shards with conflicting values
										int valueMismatches = 0;
										//The number of keys in both shards with matching values
										int matchingKVPairs = 0;
										//Last unique key on the current shard
										KeyRef currentUniqueKey;
										//Last unique key on the reference shard
										KeyRef referenceUniqueKey;
										//Last value mismatch
										KeyRef valueMismatchKey;


										//Loop indeces
										int currentI = 0;
										int referenceI = 0;
										while(currentI < current.data.size() || referenceI < reference.data.size()) {
											if(currentI >= current.data.size()) {
												referenceUniqueKey = reference.data[referenceI].key;
												referenceUniques++;
												referenceI++;
											} else if(referenceI >= reference.data.size()) {
												currentUniqueKey = current.data[currentI].key;
												currentUniques++;
												currentI++;
											} else {
												KeyValueRef currentKV = current.data[currentI];
												KeyValueRef referenceKV = reference.data[referenceI];

												if(currentKV.key == referenceKV.key) {
													if(currentKV.value == referenceKV.value)
														matchingKVPairs++;
													else {
														valueMismatchKey = currentKV.key;
														valueMismatches++;
													}

													currentI++;
													referenceI++;
												} else if(currentKV.key < referenceKV.key) {
													currentUniqueKey = currentKV.key;
													currentUniques++;
													currentI++;
												} else {
													referenceUniqueKey = referenceKV.key;
													referenceUniques++;
													referenceI++;
												}
											}
										}

										TraceEvent("ConsistencyCheck_DataInconsistent").detail(format("StorageServer%d", j).c_str(), storageServers[j].toString())
											.detail(format("StorageServer%d",firstValidServer).c_str(), storageServers[firstValidServer].toString())
											.detail("ShardBegin", printable(req.begin.getKey()))
											.detail("ShardEnd", printable(req.end.getKey()))
											.detail("VersionNumber", req.version)
											.detail(format("Server%dUniques",j).c_str(), currentUniques)
											.detail(format("Server%dUniqueKey",j).c_str(), printable(currentUniqueKey))
											.detail(format("Server%dUniques",firstValidServer).c_str(), referenceUniques)
											.detail(format("Server%dUniqueKey",firstValidServer).c_str(), printable(referenceUniqueKey))
											.detail("ValueMismatches", valueMismatches)
											.detail("ValueMismatchKey", printable(valueMismatchKey))
											.detail("MatchingKVPairs", matchingKVPairs);

										self->testFailure("Data inconsistent");
										return false;
									}
								}
							}

							//If the data is not available and we aren't relocating this shard
							else if(!isRelocating)
							{
								TraceEvent("ConsistencyCheck_StorageServerUnavailable").detail("StorageServer", storageServers[j]).detail("ShardBegin", printable(range.begin)).detail("ShardEnd", printable(range.end))
									.detail("Address", storageServerInterfaces[j].address()).detail("GetKeyValuesToken", storageServerInterfaces[j].getKeyValues.getEndpoint().token).suppressFor(1.0);

								//All shards should be available in quiscence
								if(self->performQuiescentChecks)
								{
									self->testFailure("Storage server unavailable");
									return false;
								}
							}

						}

						if(firstValidServer >= 0)
						{
							VectorRef<KeyValueRef> data = keyValueFutures[firstValidServer].get().get().data;
							//Calculate the size of the shard, the variance of the shard size estimate, and the correct shard size estimate
							for(int k = 0; k < data.size(); k++)
							{
								ByteSampleInfo sampleInfo = isKeyValueInSample(data[k]);
								shardBytes += sampleInfo.size;
								double itemProbability = ((double)sampleInfo.size) / sampleInfo.sampledSize;
								if(itemProbability < 1)
									shardVariance += itemProbability * (1 - itemProbability) * pow((double)sampleInfo.sampledSize, 2);

								if(sampleInfo.inSample) {
									sampledBytes += sampleInfo.sampledSize;
									if(!canSplit && sampledBytes >= shardBounds.min.bytes && data[k].key.size() <= CLIENT_KNOBS->SPLIT_KEY_SIZE_LIMIT && sampledBytes <= shardBounds.max.bytes*CLIENT_KNOBS->STORAGE_METRICS_UNFAIR_SPLIT_LIMIT/2  ) {
										canSplit = true;
										splitBytes = sampledBytes;
									}

									/*TraceEvent("ConsistencyCheck_ByteSample").detail("ShardBegin", printable(range.begin)).detail("ShardEnd", printable(range.end))
										.detail("SampledBytes", sampleInfo.sampledSize).detail("Key", printable(data[k].key)).detail("KeySize", data[k].key.size()).detail("ValueSize", data[k].value.size());*/

									//In data distribution, the splitting process ignores the first key in a shard.  Thus, we shouldn't consider it when validating the upper bound of estimated shard sizes
									if(k == 0)
										firstKeySampledBytes += sampleInfo.sampledSize;

									sampledKeys++;
								}
							}

							//Accumulate number of keys in this shard
							shardKeys += data.size();
						}
						//after requesting each shard, enforce rate limit based on how much data will likely be read
						if(self->rateLimit > 0)
						{
								Void _ = wait(rateLimiter->getAllowance(totalReadAmount));
						}
						bytesReadInRange += totalReadAmount;

						//Advance to the next set of entries
						if(firstValidServer >= 0 && keyValueFutures[firstValidServer].get().get().more)
						{
							VectorRef<KeyValueRef> result = keyValueFutures[firstValidServer].get().get().data;
							ASSERT(result.size() > 0);
							begin = firstGreaterThan(result[result.size() - 1].key);
							ASSERT(begin.getKey() != allKeys.end);
							lastStartSampleKey = lastSampleKey;
						}
						else
							break;
					}
					catch(Error &e)
					{
						//If we failed because of a version problem, then retry
						if(e.code() == error_code_transaction_too_old || e.code() == error_code_future_version || e.code() == error_code_transaction_too_old)
							TraceEvent("ConsistencyCheck_RetryDataConsistency").error(e);
						else
							throw;
					}
				}

				canSplit = canSplit && sampledBytes - splitBytes >= shardBounds.min.bytes && sampledBytes > splitBytes;

				//Update the size of all storage servers containing this shard
				//This is only done in a non-distributed consistency check; the distributed check uses shard size estimates
				if(!self->distributed)
					for(int j = 0; j < storageServers.size(); j++)
						storageServerSizes[storageServers[j]] += shardBytes;

				bool hasValidEstimate = estimatedBytes.size() > 0;

				//If the storage servers' sampled estimate of shard size is different from ours
				if(self->performQuiescentChecks)
				{
					for(int j = 0; j < estimatedBytes.size(); j++)
					{
						if(estimatedBytes[j] >= 0 && estimatedBytes[j] != sampledBytes)
						{
							TraceEvent("ConsistencyCheck_IncorrectEstimate").detail("EstimatedBytes", estimatedBytes[j]).detail("CorrectSampledBytes", sampledBytes)
								.detail("StorageServer", storageServers[j]);
							self->testFailure("Storage servers had incorrect sampled estimate");

							hasValidEstimate = false;

							break;
						}
						else if(estimatedBytes[j] < 0)
						{
							self->testFailure("Could not get storage metrics from server");
							hasValidEstimate = false;
							break;
						}
					}
				}

				//Compute the difference between the shard size estimate and its actual size.  If it is sufficiently large, then fail
				double stdDev = sqrt(shardVariance);

				double failErrorNumStdDev = 7;
				int estimateError = abs(shardBytes - sampledBytes);

				//Only perform the check if there are sufficient keys to get a distribution that should resemble a normal distribution
				if(sampledKeys > 30 && estimateError > failErrorNumStdDev * stdDev)
				{
					double numStdDev = estimateError / sqrt(shardVariance);
					TraceEvent("ConsistencyCheck_InaccurateShardEstimate").detail("Min", shardBounds.min.bytes).detail("Max", shardBounds.max.bytes).detail("Estimate", sampledBytes)
						.detail("Actual", shardBytes).detail("NumStdDev", numStdDev).detail("Variance", shardVariance).detail("StdDev", stdDev)
						.detail("ShardBegin", printable(range.begin)).detail("ShardEnd", printable(range.end)).detail("NumKeys", shardKeys).detail("NumSampledKeys", sampledKeys);

					self->testFailure(format("Shard size is more than %f std dev from estimate", failErrorNumStdDev));
				}

				//In a quiescent database, check that the (estimated) size of the shard is within permitted bounds
				//Min and max shard sizes have a 3 * shardBounds.permittedError.bytes cushion for error since shard sizes are not precise
				//Shard splits ignore the first key in a shard, so its size shouldn't be considered when checking the upper bound
				//0xff shards are not checked
				if( canSplit && sampledKeys > 5 && self->performQuiescentChecks && !range.begin.startsWith(keyServersPrefix) &&
					(sampledBytes < shardBounds.min.bytes - 3 * shardBounds.permittedError.bytes || sampledBytes - firstKeySampledBytes > shardBounds.max.bytes + 3 * shardBounds.permittedError.bytes))
				{
					TraceEvent("ConsistencyCheck_InvalidShardSize").detail("Min", shardBounds.min.bytes).detail("Max", shardBounds.max.bytes).detail("Size", shardBytes)
						.detail("EstimatedSize", sampledBytes).detail("ShardBegin", printable(range.begin)).detail("ShardEnd", printable(range.end)).detail("ShardCount", ranges.size())
						.detail("SampledKeys", sampledKeys);
					self->testFailure(format("Shard size in quiescent database is too %s", (sampledBytes < shardBounds.min.bytes) ? "small" : "large"));
					return false;
				}
			}

			if(bytesReadInRange > 0) {
				TraceEvent("ConsistencyCheck_ReadRange").detail("range", printable(range)).detail("bytesRead", bytesReadInRange);
			}
		}

		//SOMEDAY: when background data distribution is implemented, include this test
		//In a quiescent database, check that the sizes of storage servers are roughly the same
		/*if(self->performQuiescentChecks)
		{
			auto minStorageServer = std::min_element(storageServerSizes.begin(), storageServerSizes.end(), ConsistencyCheckWorkload::compareByValue<UID, int64_t>);
			auto maxStorageServer = std::max_element(storageServerSizes.begin(), storageServerSizes.end(), ConsistencyCheckWorkload::compareByValue<UID, int64_t>);

			int bias = SERVER_KNOBS->MIN_SHARD_BYTES;
			if(1.1 * (minStorageServer->second + SERVER_KNOBS->MIN_SHARD_BYTES) < maxStorageServer->second + SERVER_KNOBS->MIN_SHARD_BYTES)
			{
				TraceEvent("ConsistencyCheck_InconsistentStorageServerSizes").detail("MinSize", minStorageServer->second).detail("MaxSize", maxStorageServer->second)
					.detail("MinStorageServer", minStorageServer->first).detail("MaxStorageServer", maxStorageServer->first);

				self->testFailure(format("Storage servers differ significantly in size by a factor of %f", ((double)maxStorageServer->second) / minStorageServer->second));
				return false;
			}
		}*/


		return true;
	}

	//Returns true if any storage servers have the exact same network address or are not using the correct key value store type
	ACTOR Future<bool> checkForUndesirableServers(Database cx, DatabaseConfiguration configuration, ConsistencyCheckWorkload *self)
	{
		state int i;
		state int j;
		state vector<StorageServerInterface> storageServers = wait( getStorageServers( cx ) );
		//Check each pair of storage servers for an address match
		for(i = 0; i < storageServers.size(); i++)
		{
			//Check that each storage server has the correct key value store type
			ReplyPromise<KeyValueStoreType> typeReply;
			ErrorOr<KeyValueStoreType> keyValueStoreType = wait(storageServers[i].getKeyValueStoreType.getReplyUnlessFailedFor(typeReply, 2, 0));

			if(!keyValueStoreType.present())
			{
				TraceEvent("ConsistencyCheck_ServerUnavailable").detail("ServerID", storageServers[i].id());
				self->testFailure("Storage server unavailable");
			}
			else if(keyValueStoreType.get() != configuration.storageServerStoreType)
			{
				TraceEvent("ConsistencyCheck_WrongKeyValueStoreType").detail("ServerID", storageServers[i].id()).detail("StoreType", keyValueStoreType.get().toString()).detail("DesiredType", configuration.storageServerStoreType.toString());
				self->testFailure("Storage server has wrong key-value store type");
				return true;
			}

			//Check each pair of storage servers for an address match
			for(j = i + 1; j < storageServers.size(); j++)
			{
				if(storageServers[i].address() == storageServers[j].address())
				{
					TraceEvent("ConsistencyCheck_UndesirableServer").detail("StorageServer1", storageServers[i].id()).detail("StorageServer2", storageServers[j].id())
						.detail("Address", storageServers[i].address());
					self->testFailure("Multiple storage servers have the same address");
					return true;
				}
			}
		}

		return false;
	}

	//Returns false if any worker that should have a storage server does not have one
	ACTOR Future<bool> checkForStorage(Database cx, DatabaseConfiguration configuration, ConsistencyCheckWorkload *self)
	{
		state vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( self->dbInfo ) );
		state vector<StorageServerInterface> storageServers = wait( getStorageServers( cx ) );

		for( int i = 0; i < workers.size(); i++ ) {
			if( !configuration.isExcludedServer(workers[i].first.address()) &&
				( workers[i].second == ProcessClass::StorageClass || workers[i].second == ProcessClass::UnsetClass ) ) {
				bool found = false;
				for( int j = 0; j < storageServers.size(); j++ ) {
					if( storageServers[j].address() == workers[i].first.address() ) {
						found = true;
						break;
					}
				}
				if( !found ) {
					TraceEvent("ConsistencyCheck_NoStorage").detail("Address", workers[i].first.address());
					self->testFailure("No storage server on worker");
					return false;
				}
			}
		}
		return true;
	}

	ACTOR Future<bool> checkForExtraDataStores(Database cx, ConsistencyCheckWorkload *self) {
		state vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( self->dbInfo ) );
		state vector<StorageServerInterface> storageServers = wait( getStorageServers( cx ) );
		auto& db = self->dbInfo->get();
		state std::vector<TLogInterface> logs = db.logSystemConfig.allPresentLogs();

		state std::vector<std::pair<WorkerInterface, ProcessClass>>::iterator itr;
		state bool foundExtraDataStore = false;

		state std::map<NetworkAddress, std::set<UID>> statefulProcesses;
		for(auto ss : storageServers) {
			statefulProcesses[ss.address()].insert(ss.id());
		}
		for(auto log : logs) {
			statefulProcesses[log.address()].insert(log.id());
		}

		for(itr = workers.begin(); itr != workers.end(); ++itr) {
			ErrorOr<Standalone<VectorRef<UID>>> stores = wait(itr->first.diskStoreRequest.getReplyUnlessFailedFor(DiskStoreRequest(false), 2, 0));
			if(stores.isError()) {
				TraceEvent("ConsistencyCheck_GetDataStoreFailure").detail("Address", itr->first.address()).error(stores.getError());
				self->testFailure("Failed to get data stores");
				return false;
			}

			for(auto id : stores.get()) {
				if(!statefulProcesses[itr->first.address()].count(id)) {
					TraceEvent("ConsistencyCheck_ExtraDataStore").detail("Address", itr->first.address()).detail("DataStoreID", id);
					if(g_network->isSimulated()) {
						TraceEvent("ConsistencyCheck_RebootProcess").detail("Address", itr->first.address()).detail("DataStoreID", id);
						g_simulator.rebootProcess(g_simulator.getProcessByAddress(itr->first.address()), ISimulator::RebootProcess);
					}

					foundExtraDataStore = true;
				}
			}
		}

		if(foundExtraDataStore) {
			self->testFailure("Extra data stores present on workers");
			return false;
		}

		return true;
	}

	//Returns true if the worker at the given address has the specified machineClass or has an unset class
	//The interfaceType paramater is used in a TraceEvent, should be something like (Master, MasterProxy, StorageServer, ...)
	bool workerHasClass(vector<std::pair<WorkerInterface, ProcessClass>> workers, NetworkAddress address, ProcessClass::ClassType machineClass, std::string interfaceType)
	{
		//Search all workers until the correct one is found
		for(int i = 0; i < workers.size(); i++)
		{
			if(workers[i].first.address() == address)
			{
				if(workers[i].second == machineClass || workers[i].second == ProcessClass::UnsetClass)
					return true;

				TraceEvent("ConsistencyCheck_InvalidClassType").detail("RequestedClass", workers[i].second.toString())
					.detail("ActualClass", ProcessClass(machineClass, ProcessClass::CommandLineSource).toString()).detail("InterfaceType", interfaceType);

				return false;
			}
		}

		//No worker had specified address
		TraceEvent("ConsistencyCheck_WorkerNotFound").detail("Address", address).detail("ActualClass", ProcessClass(machineClass, ProcessClass::CommandLineSource).toString())
			.detail("InterfaceType", interfaceType);

		return false;
	}

	ACTOR Future<bool> checkWorkerList( Database cx, ConsistencyCheckWorkload *self ) {
		if(g_simulator.extraDB)
			return true;

		vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( self->dbInfo ) );
		std::set<NetworkAddress> workerAddresses;

		for( auto it : workers ) {
			ISimulator::ProcessInfo* info = g_simulator.getProcessByAddress(it.first.address());
			if(!info || info->failed) {
				TraceEvent("ConsistencyCheck_FailedWorkerInList").detail("addr", it.first.address());
				return false;
			}
			workerAddresses.insert( NetworkAddress(it.first.address().ip, it.first.address().port, true, false) );
		}

		vector<ISimulator::ProcessInfo*> all = g_simulator.getAllProcesses();
		for(int i = 0; i < all.size(); i++) {
			if( all[i]->isReliable() && all[i]->name == std::string("Server") && all[i]->startingClass != ProcessClass::TesterClass ) {
				if(!workerAddresses.count(all[i]->address)) {
					TraceEvent("ConsistencyCheck_WorkerMissingFromList").detail("addr", all[i]->address);
					return false;
				}
			}
		}

		return true;
	}

	static ProcessClass::Fitness getBestAvailableFitness(std::set<ProcessClass::ClassType>& availableClassTypes, ProcessClass::ClusterRole role) {
		ProcessClass::Fitness bestAvailableFitness = ProcessClass::NeverAssign;
		for (auto classType : availableClassTypes) {
			bestAvailableFitness = std::min(bestAvailableFitness, ProcessClass(classType, ProcessClass::InvalidSource).machineClassFitness(role));
		}

		return bestAvailableFitness;
	}

	//Returns true if all machines in the cluster that specified a desired class are operating in that class
	ACTOR Future<bool> checkUsingDesiredClasses(Database cx, ConsistencyCheckWorkload *self)
	{
		state vector<std::pair<WorkerInterface, ProcessClass>> allWorkers = wait( getWorkers( self->dbInfo ) );
		state vector<std::pair<WorkerInterface, ProcessClass>> nonExcludedWorkers = wait( getWorkers( self->dbInfo, GetWorkersRequest::NON_EXCLUDED_PROCESSES_ONLY ) );
		state vector<StorageServerInterface> storageServers = wait( getStorageServers( cx ) );
		auto& db = self->dbInfo->get();

		std::set<ProcessClass::ClassType> allClassTypes;
		std::map<NetworkAddress, ProcessClass> allWorkerProcessMap;
		for (auto worker : allWorkers) {
			allClassTypes.insert(worker.second.classType());
			allWorkerProcessMap[worker.first.address()] = worker.second;
		}

		std::set<ProcessClass::ClassType> nonExcludedClassTypes;
		std::map<NetworkAddress, ProcessClass> nonExcludedWorkerProcessMap;
		for (auto worker : nonExcludedWorkers) {
			nonExcludedClassTypes.insert(worker.second.classType());
			nonExcludedWorkerProcessMap[worker.first.address()] = worker.second;
		}

		// Check cluster controller
		ProcessClass::Fitness bestClusterControllerFitness = getBestAvailableFitness(nonExcludedClassTypes, ProcessClass::ClusterController);
		if (!nonExcludedWorkerProcessMap.count(db.clusterInterface.clientInterface.address()) || nonExcludedWorkerProcessMap[db.clusterInterface.clientInterface.address()].machineClassFitness(ProcessClass::ClusterController) != bestClusterControllerFitness) {
			TraceEvent("ConsistencyCheck_ClusterControllerNotBest").detail("bestClusterControllerFitness", bestClusterControllerFitness).detail("existingClusterControllerFit", nonExcludedWorkerProcessMap.count(db.clusterInterface.clientInterface.address()) ? nonExcludedWorkerProcessMap[db.clusterInterface.clientInterface.address()].machineClassFitness(ProcessClass::ClusterController) : -1);
			return false;
		}

		// Check master
		ProcessClass::Fitness bestMasterFitness = getBestAvailableFitness(nonExcludedClassTypes, ProcessClass::Master);
		if (bestMasterFitness == ProcessClass::NeverAssign) {
			bestMasterFitness = getBestAvailableFitness(allClassTypes, ProcessClass::Master);
			if (bestMasterFitness != ProcessClass::NeverAssign) {
				bestMasterFitness = ProcessClass::ExcludeFit;
			}
		}

		if (!allWorkerProcessMap.count(db.master.address()) || (!nonExcludedWorkerProcessMap.count(db.master.address()) && bestMasterFitness != ProcessClass::ExcludeFit) || nonExcludedWorkerProcessMap[db.master.address()].machineClassFitness(ProcessClass::Master) != bestMasterFitness) {
			TraceEvent("ConsistencyCheck_MasterNotBest").detail("bestMasterFitness", bestMasterFitness).detail("existingMasterFit", nonExcludedWorkerProcessMap.count(db.master.address()) ? nonExcludedWorkerProcessMap[db.master.address()].machineClassFitness(ProcessClass::Master) : -1);
			return false;
		}

		// Check master proxy
		ProcessClass::Fitness bestMasterProxyFitness = getBestAvailableFitness(nonExcludedClassTypes, ProcessClass::Proxy);
		for (auto masterProxy : db.client.proxies) {
			if (!nonExcludedWorkerProcessMap.count(masterProxy.address()) || nonExcludedWorkerProcessMap[masterProxy.address()].machineClassFitness(ProcessClass::Proxy) != bestMasterProxyFitness) {
				TraceEvent("ConsistencyCheck_ProxyNotBest").detail("bestMasterProxyFitness", bestMasterProxyFitness).detail("existingMasterProxyFitness", nonExcludedWorkerProcessMap.count(masterProxy.address()) ? nonExcludedWorkerProcessMap[masterProxy.address()].machineClassFitness(ProcessClass::Proxy) : -1);
				return false;
			}
		}

		// Check resolver
		ProcessClass::Fitness bestResolverFitness = getBestAvailableFitness(nonExcludedClassTypes, ProcessClass::Resolver);
		for (auto resolver : db.resolvers) {
			if (!nonExcludedWorkerProcessMap.count(resolver.address()) || nonExcludedWorkerProcessMap[resolver.address()].machineClassFitness(ProcessClass::Resolver) != bestResolverFitness) {
				TraceEvent("ConsistencyCheck_ResolverNotBest").detail("bestResolverFitness", bestResolverFitness).detail("existingResolverFitness", nonExcludedWorkerProcessMap.count(resolver.address()) ? nonExcludedWorkerProcessMap[resolver.address()].machineClassFitness(ProcessClass::Resolver) : -1);
				return false;
			}
		}

		// TODO: Check Tlog

		return true;
	}
};

WorkloadFactory<ConsistencyCheckWorkload> ConsistencyCheckWorkloadFactory("ConsistencyCheck");
