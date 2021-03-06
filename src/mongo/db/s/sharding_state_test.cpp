/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"


#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

class ShardingStateTest : public MongodTestFixture {
public:
    ShardingState* shardingState() {
        return &_shardingState;
    }

protected:
    void setUp() override {
        MongodTestFixture::setUp();

        auto serviceContext = getServiceContext();
        serviceContext->setFastClockSource(stdx::make_unique<ClockSourceMock>());
        serviceContext->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
        serviceContext->setTickSource(stdx::make_unique<TickSourceMock>());

        // When sharding initialization is triggered, initialize sharding state as a shard server.
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _shardingState.setGlobalInitMethodForTest([&](OperationContext* txn,
                                                      const ConnectionString& configConnStr,
                                                      StringData distLockProcessId) {
            auto status = initializeGlobalShardingStateForMongodForTest(configConnStr);
            if (!status.isOK()) {
                return status;
            }
            // Set the ConnectionString return value on the mock targeter so that later calls to
            // the targeter's getConnString() return the appropriate value.
            auto configTargeter =
                RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
            configTargeter->setConnectionStringReturnValue(configConnStr);

            return Status::OK();
        });
    }

    void tearDown() override {
        // ShardingState initialize can modify ReplicaSetMonitor state.
        ReplicaSetMonitor::cleanup();
        MongodTestFixture::tearDown();
    }

private:
    ShardingState _shardingState;
};

TEST_F(ShardingStateTest, ValidShardIdentitySucceeds) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));
    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitWhilePreviouslyInErrorStateWillStayInErrorState) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::ShutdownInProgress, "shutting down"};
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ShutdownInProgress, status);
    }

    // ShardingState is now in error state, attempting to call it again will still result in error.

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status::OK();
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ManualInterventionRequired, status);
    }

    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, InitializeAgainWithMatchingShardIdentitySucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithSameReplSetNameSucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "b:2,c:3", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentReplSetNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "configRS"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentShardNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("b");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentClusterIdFails) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

}  // namespace mongo
