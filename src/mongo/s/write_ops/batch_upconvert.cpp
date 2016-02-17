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

#include "mongo/s/write_ops/batch_upconvert.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/multi_command_dispatch.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/batched_update_document.h"

namespace mongo {

    using mongoutils::str::stream;

    BatchedCommandRequest* msgToBatchRequest( const Message& msg ) {

        int opType = msg.operation();

        auto_ptr<BatchedCommandRequest> request;
        if ( opType == dbInsert ) {
            request.reset( msgToBatchInsert( msg ) );
        }
        else if ( opType == dbUpdate ) {
            request.reset( msgToBatchUpdate( msg ) );
        }
        else {
            dassert( opType == dbDelete );
            request.reset( msgToBatchDelete( msg ) );
        }

        return request.release();
    }

    BatchedCommandRequest* msgToBatchInsert( const Message& insertMsg ) {

        // Parsing DbMessage throws
        DbMessage dbMsg( insertMsg );
        NamespaceString nss( dbMsg.getns() );
        bool coe = dbMsg.reservedField() & Reserved_InsertOption_ContinueOnError;

        vector<BSONObj> docs;
        do {
            docs.push_back( dbMsg.nextJsObj() );
        }
        while ( dbMsg.moreJSObjs() );

        // Continue-on-error == unordered
        bool ordered = !coe;

        // No exceptions from here on
        BatchedCommandRequest* request =
            new BatchedCommandRequest( BatchedCommandRequest::BatchType_Insert );
        request->setNS( nss.ns() );
        for ( vector<BSONObj>::const_iterator it = docs.begin(); it != docs.end(); ++it ) {
            request->getInsertRequest()->addToDocuments( *it );
        }
        request->setOrdered( ordered );

        return request;
    }

    BatchedCommandRequest* msgToBatchUpdate( const Message& updateMsg ) {

        // Parsing DbMessage throws
        DbMessage dbMsg( updateMsg );
        NamespaceString nss( dbMsg.getns() );
        int flags = dbMsg.pullInt();
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        const BSONObj query = dbMsg.nextJsObj();
        const BSONObj updateExpr = dbMsg.nextJsObj();

        // No exceptions from here on
        BatchedUpdateDocument* updateDoc = new BatchedUpdateDocument;
        updateDoc->setQuery( query );
        updateDoc->setUpdateExpr( updateExpr );
        updateDoc->setUpsert( upsert );
        updateDoc->setMulti( multi );

        BatchedCommandRequest* request =
            new BatchedCommandRequest( BatchedCommandRequest::BatchType_Update );
        request->setNS( nss.ns() );
        request->getUpdateRequest()->addToUpdates( updateDoc );

        return request;
    }

    BatchedCommandRequest* msgToBatchDelete( const Message& deleteMsg ) {

        // Parsing DbMessage throws
        DbMessage dbMsg( deleteMsg );
        NamespaceString nss( dbMsg.getns() );
        int flags = dbMsg.pullInt();
        const BSONObj query = dbMsg.nextJsObj();
        int limit = ( flags & RemoveOption_JustOne ) ? 1 : 0;

        // No exceptions from here on
        BatchedDeleteDocument* deleteDoc = new BatchedDeleteDocument;
        deleteDoc->setLimit( limit );
        deleteDoc->setQuery( query );

        BatchedCommandRequest* request =
            new BatchedCommandRequest( BatchedCommandRequest::BatchType_Delete );
        request->setNS( nss.ns() );
        request->getDeleteRequest()->addToDeletes( deleteDoc );

        return request;
    }

    void buildErrorFromResponse( const BatchedCommandResponse& response, BatchedErrorDetail* error ) {
        error->setErrCode( response.getErrCode() );
        if ( error->isErrInfoSet() ) error->setErrInfo( response.getErrInfo() );
        error->setErrMessage( response.getErrMessage() );
    }

    void batchErrorToLastError( const BatchedCommandRequest& request,
                                const BatchedCommandResponse& response,
                                LastError* error ) {

        scoped_ptr<BatchedErrorDetail> topLevelError;
        BatchedErrorDetail* lastBatchError = NULL;

        if ( !response.getOk() ) {

            int code = response.getErrCode();

            // Check for batch error
            // We don't care about write concern errors, these happen in legacy mode in GLE
            if ( code != ErrorCodes::WriteConcernFailed && !response.isErrDetailsSet() ) {
                // Top-level error, all writes failed
                topLevelError.reset( new BatchedErrorDetail );
                buildErrorFromResponse( response, topLevelError.get() );
                lastBatchError = topLevelError.get();
            }
            else if ( response.isErrDetailsSet() ) {
                // The last error in the batch is always reported - this matches expected COE
                // semantics for insert batches and works for single writes
                lastBatchError = response.getErrDetails().back();
            }
        }

        // Record an error if one exists
        if ( lastBatchError ) {
            error->raiseError( lastBatchError->getErrCode(),
                               lastBatchError->getErrMessage().c_str() );
            return;
        }

        // Record write stats otherwise
        // NOTE: For multi-write batches, our semantics change a little because we don't have
        // un-aggregated "n" stats.
        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update ) {

            BSONObj upsertedId;
            if ( response.isSingleUpsertedSet() ) upsertedId = response.getSingleUpserted();
            else if( response.isUpsertDetailsSet() ) {
                // Only report the very last item's upserted id if applicable
                if ( response.getUpsertDetails().back()->getIndex() + 1
                     == static_cast<int>( request.sizeWriteOps() ) ) {
                    upsertedId = response.getUpsertDetails().back()->getUpsertedID();
                }
            }

            int numUpserted = 0;
            if ( response.isSingleUpsertedSet() )
                ++numUpserted;
            else if ( response.isUpsertDetailsSet() )
                numUpserted += response.sizeUpsertDetails();

            int numUpdated = response.getN() - numUpserted;
            dassert( numUpdated >= 0 );
            error->recordUpdate( numUpdated > 0, response.getN(), upsertedId );
        }
        else if ( request.getBatchType() == BatchedCommandRequest::BatchType_Delete ) {
            error->recordDelete( response.getN() );
        }
    }
}
