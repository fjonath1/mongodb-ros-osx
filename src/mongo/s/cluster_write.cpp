/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_write.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/config.h"
#include "mongo/s/dbclient_multi_command.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/config_coordinator.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    using std::vector;
    using std::string;

    /**
     * Splits the chunks touched based from the targeter stats if needed.
     */
    static void splitIfNeeded( const string& ns, const TargeterStats& stats ) {
        if ( !Chunk::ShouldAutoSplit ) {
            return;
        }

        DBConfigPtr config;

        try {
            config = grid.getDBConfig( ns );
        }
        catch ( const DBException& ex ) {
            warning() << "failed to get database config for " << ns
                      << " while checking for auto-split: " << causedBy( ex ) << endl;
            return;
        }

        ChunkManagerPtr chunkManager;
        ShardPtr dummyShard;
        config->getChunkManagerOrPrimary( ns, chunkManager, dummyShard );

        if ( !chunkManager ) {
            return;
        }

        for ( map<BSONObj, int>::const_iterator it = stats.chunkSizeDelta.begin();
            it != stats.chunkSizeDelta.end(); ++it ) {

            ChunkPtr chunk;
            try {
                chunk = chunkManager->findIntersectingChunk( it->first );
            }
            catch ( const AssertionException& ex ) {
                warning() << "could not find chunk while checking for auto-split: "
                          << causedBy( ex ) << endl;
                return;
            }

            chunk->splitIfShould( it->second );
        }
    }

    /**
     * Returns the currently-set config hosts for a cluster
     */
    static vector<ConnectionString> getConfigHosts() {

        vector<ConnectionString> configHosts;
        ConnectionString configHostOrHosts = configServer.getConnectionString();
        if ( configHostOrHosts.type() == ConnectionString::MASTER ) {
            configHosts.push_back( configHostOrHosts );
        }
        else {
            dassert( configHostOrHosts.type() == ConnectionString::SYNC );
            vector<HostAndPort> configHPs = configHostOrHosts.getServers();
            for ( vector<HostAndPort>::iterator it = configHPs.begin(); it != configHPs.end();
                ++it ) {
                configHosts.push_back( ConnectionString( *it ) );
            }
        }

        return configHosts;
    }

    static void shardWrite( const BatchedCommandRequest& request,
                            BatchedCommandResponse* response,
                            bool autoSplit ) {

        ChunkManagerTargeter targeter;
        Status targetInitStatus = targeter.init( NamespaceString( request.getTargetingNS() ) );

        if ( !targetInitStatus.isOK() ) {

            warning() << "could not initialize targeter for"
                      << ( request.isInsertIndexRequest() ? " index" : "" )
                      << " write op in collection " << request.getTargetingNS() << endl;

            // Errors will be reported in response if we are unable to target
        }

        DBClientShardResolver resolver;
        DBClientMultiCommand dispatcher;
        BatchWriteExec exec( &targeter, &resolver, &dispatcher );
        exec.executeBatch( request, response );

        if ( autoSplit ) splitIfNeeded( request.getNS(), *targeter.getStats() );
    }

    static void configWrite( const BatchedCommandRequest& request,
                             BatchedCommandResponse* response,
                             bool fsyncCheck ) {

        DBClientMultiCommand dispatcher;
        ConfigCoordinator exec( &dispatcher, getConfigHosts() );
        exec.executeBatch( request, response, fsyncCheck );
    }

    void clusterWrite( const BatchedCommandRequest& request,
                       BatchedCommandResponse* response,
                       bool autoSplit ) {

        // App-level validation of a create index insert
        if ( request.isInsertIndexRequest() ) {
            if ( request.sizeWriteOps() != 1 || request.isWriteConcernSet() ) {

                // Invalid request to create index
                response->setOk( false );
                response->setErrCode( ErrorCodes::InvalidOptions );
                response->setErrMessage( "invalid batch request for index creation" );

                dassert( response->isValid( NULL ) );
                return;
            }
        }

        // Config writes and shard writes are done differently
        string dbName = NamespaceString( request.getNS() ).db().toString();
        if ( dbName == "config" || dbName == "admin" ) {

            bool verboseWC = request.isVerboseWC();

            // We only support batch sizes of one and {w:0} write concern for config writes
            if ( request.sizeWriteOps() != 1 || ( verboseWC && request.isWriteConcernSet() ) ) {
                // Invalid config server write
                response->setOk( false );
                response->setErrCode( ErrorCodes::InvalidOptions );
                response->setErrMessage( "invalid batch request for config write" );

                dassert( response->isValid( NULL ) );
                return;
            }

            // We need to support "best-effort" writes for pings to the config server.
            // {w:0} (!verbose) writes are interpreted as best-effort in this case - they may still
            // error, but do not do the initial fsync check.
            configWrite( request, response, verboseWC );
        }
        else {
            shardWrite( request, response, autoSplit );
        }
    }

} // namespace mongo
