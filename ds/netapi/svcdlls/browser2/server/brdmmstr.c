
/*++

Copyright (c) 1991  Microsoft Corporation

Module Name:

    brdmmstr.c

Abstract:

    This module contains the routines to manage a domain master browser server

Author:

    Rita Wong (ritaw) 20-Feb-1991

Revision History:

--*/

#include "precomp.h"
#pragma hdrstop

//-------------------------------------------------------------------//
//                                                                   //
// Local structure definitions                                       //
//                                                                   //
//-------------------------------------------------------------------//

//-------------------------------------------------------------------//
//                                                                   //
// Local function prototypes                                         //
//                                                                   //
//-------------------------------------------------------------------//


VOID
BrPostGetMasterAnnouncementWorker(
    IN PVOID Ctx
    );

VOID
GetMasterAnnouncementCompletion (
    IN PVOID Ctx
    );

typedef struct _BROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT {
    PDOMAIN_INFO DomainInfo;
    HANDLE EventHandle;
    NET_API_STATUS NetStatus;
} BROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT, *PBROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT;

NET_API_STATUS
BrPostGetMasterAnnouncementInWorker(
    PDOMAIN_INFO DomainInfo
    )

/*++

Routine Description:

    Wrapper for BrPostGetMasterAnnouncement.  Since BrPostGetMasterAnnouncement
    starts a pending IO to the browser driver, the thread that calls
    BrPostGetMasterAnnouncement must remain around forever.  This wrapper can
    be called by any transient thread (e.g., an RPC thread).  It simply causes
    BrPostGetMasterAnnouncement to be called in a worker thread.

Arguments:

    DomainInfo - Domain the get master announcement is to be posted for

Return Value:

    Status of operation.

--*/
{
    NET_API_STATUS NetStatus;
    DWORD WaitStatus;

    WORKER_ITEM WorkItem;
    BROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT Context;

    //
    // Copy the parameters into the context.
    //

    Context.DomainInfo = DomainInfo;

    //
    // Create an event which we use to wait on the worker thread.
    //

    Context.EventHandle = CreateEvent(
                 NULL,                // Event attributes
                 TRUE,                // Event must be manually reset
                 FALSE,               // Initial state not signalled
                 NULL );              // Event name

    if ( Context.EventHandle == NULL ) {
        NetStatus = GetLastError();
        return NetStatus;
    }

    //
    // Queue the request to the worker thread.
    //

    BrInitializeWorkItem( &WorkItem,
                          BrPostGetMasterAnnouncementWorker,
                          &Context );

    BrQueueWorkItem( &WorkItem );

    //
    // Wait for the worker thread to finish
    //

    WaitStatus = WaitForSingleObject( Context.EventHandle, INFINITE );

    if ( WaitStatus == WAIT_OBJECT_0 ) {
        NetStatus = Context.NetStatus;
    } else {
        NetStatus = GetLastError();
    }

    CloseHandle( Context.EventHandle );

    return NetStatus;
}

VOID
BrPostGetMasterAnnouncementWorker(
    IN PVOID Ctx
    )
/*++

Routine Description:

    Worker routine for BrPostGetMasterAnnouncementInWorker.

    This routine executes in the context of a worker thread.

Arguments:

    Context - Context describing the workitem.

Return Value:

    None

--*/
{
    PBROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT Context =
        (PBROWSER_GET_MASTER_ANNOUNCEMENT_CONTEXT) Ctx;

    //
    // Create the domain.
    //

    Context->NetStatus = BrPostGetMasterAnnouncement( Context->DomainInfo );

    //
    // Let the caller know we're done.
    //
    SetEvent( Context->EventHandle );

}

NET_API_STATUS
BrPostGetMasterAnnouncement (
    PDOMAIN_INFO DomainInfo
    )
/*++

Routine Description:

    Ensure the GetMasterAnnouncement request is posted for all networks on
    the specified domain.

Arguments:

    DomainInfo - Domain this operation is to apply to

Return Value:

    Status - The status of the operation.

--*/
{
    BrPrint(( BR_MASTER, "%ws: BrPostGetMasterAnnouncement\n", DomainInfo->DomUnicodeDomainName ));

    return BrEnumerateNetworksForDomain(DomainInfo, PostGetMasterAnnouncement, NULL);
}


NET_API_STATUS
PostGetMasterAnnouncement (
    PNETWORK Network,
    PVOID Context
    )
/*++

Routine Description:

    Ensure the GetMasterAnnouncement request is posted for a particular network.

Arguments:

    None.

Return Value:

    Status - The status of the operation.

--*/
{
    NET_API_STATUS NetStatus = NERR_Success;

    if (!LOCK_NETWORK(Network)) {
        return NERR_InternalError;
    }

    if ( Network->DomainInfo->IsDomainMasterBrowser &&
        (Network->Flags & NETWORK_WANNISH)) {

        if (!(Network->Flags & NETWORK_GET_MASTER_ANNOUNCE_POSTED)) {

            BrPrint(( BR_MASTER,
                      "%ws: PostGetMasterAnnouncement on %ws\n",
                      Network->DomainInfo->DomUnicodeDomainName,
                      Network->NetworkName.Buffer));

            NetStatus = BrIssueAsyncBrowserIoControl(Network,
                                        IOCTL_LMDR_WAIT_FOR_MASTER_ANNOUNCE,
                                        GetMasterAnnouncementCompletion,
                                        NULL
                                        );

            if ( NetStatus == NERR_Success ) {
                Network->Flags |= NETWORK_GET_MASTER_ANNOUNCE_POSTED;
            }
        } else {
            BrPrint(( BR_MASTER,
                      "%ws: PostGetMasterAnnouncement already posted on %ws\n",
                      Network->DomainInfo->DomUnicodeDomainName,
                      Network->NetworkName.Buffer));
        }
    }

    UNLOCK_NETWORK(Network);
    return NetStatus;

    UNREFERENCED_PARAMETER(Context);
}


VOID
GetMasterAnnouncementCompletion (
    IN PVOID Ctx
    )
/*++

Routine Description:

    This function is the completion routine for a master announcement.  It is
    called whenever a master announcement is received for a particular network.

Arguments:

    Ctx - Context block for request.

Return Value:

    None.

--*/


{
    PVOID ServerList = NULL;
    ULONG EntriesRead;
    ULONG TotalEntries;
    NET_API_STATUS Status = NERR_Success;
    PBROWSERASYNCCONTEXT Context = Ctx;
    PLMDR_REQUEST_PACKET MasterAnnouncement = Context->RequestPacket;
    PNETWORK Network = Context->Network;
    LPTSTR RemoteMasterName = NULL;
    BOOLEAN NetLocked = FALSE;
    BOOLEAN NetReferenced = FALSE;


    try {
        //
        // Ensure the network wasn't deleted from under us.
        //
        if ( BrReferenceNetwork( Network ) == NULL ) {
            try_return(NOTHING);
        }
        NetReferenced = TRUE;

        if (!LOCK_NETWORK(Network)){
            try_return(NOTHING);
        }
        NetLocked = TRUE;

        Network->Flags &= ~NETWORK_GET_MASTER_ANNOUNCE_POSTED;

        //
        //  The request failed for some reason - just return immediately.
        //

        if (!NT_SUCCESS(Context->IoStatusBlock.Status)) {
            try_return(NOTHING);
        }

        Status = PostGetMasterAnnouncement(Network, NULL);

        if (Status != NERR_Success) {
            BrPrint(( BR_CRITICAL,
                      "%ws: %ws: Unable to re-issue GetMasterAnnouncement request: %lx\n",
                      Network->DomainInfo->DomUnicodeDomainName,
                      Network->NetworkName.Buffer,
                      Status));

            try_return(NOTHING);

        }


        RemoteMasterName = MIDL_user_allocate(MasterAnnouncement->Parameters.WaitForMasterAnnouncement.MasterNameLength+3*sizeof(TCHAR));

        if (RemoteMasterName == NULL) {
            try_return(NOTHING);
        }

        RemoteMasterName[0] = TEXT('\\');
        RemoteMasterName[1] = TEXT('\\');

        STRNCPY(&RemoteMasterName[2],
                MasterAnnouncement->Parameters.WaitForMasterAnnouncement.Name,
                MasterAnnouncement->Parameters.WaitForMasterAnnouncement.MasterNameLength/sizeof(TCHAR));

        RemoteMasterName[(MasterAnnouncement->Parameters.WaitForMasterAnnouncement.MasterNameLength/sizeof(TCHAR))+2] = UNICODE_NULL;

        BrPrint(( BR_MASTER,
                  "%ws: %ws: GetMasterAnnouncement: Got a master browser announcement from %ws\n",
                  Network->DomainInfo->DomUnicodeDomainName,
                  Network->NetworkName.Buffer,
                  RemoteMasterName));

        UNLOCK_NETWORK(Network);

        NetLocked = FALSE;

        //
        //  Remote the api and pull the browse list from the remote server.
        //

        Status = RxNetServerEnum(RemoteMasterName,
                                     Network->NetworkName.Buffer,
                                     101,
                                     (LPBYTE *)&ServerList,
                                     0xffffffff,
                                     &EntriesRead,
                                     &TotalEntries,
                                     SV_TYPE_LOCAL_LIST_ONLY,
                                     NULL,
                                     NULL
                                     );

        if ((Status == NERR_Success) || (Status == ERROR_MORE_DATA)) {

            if (!LOCK_NETWORK(Network)) {
                try_return(NOTHING);
            }

            NetLocked = TRUE;

            Status = MergeServerList(&Network->BrowseTable,
                                     101,
                                     ServerList,
                                     EntriesRead,
                                     TotalEntries
                                     );

            UNLOCK_NETWORK(Network);

            NetLocked = FALSE;

            (void) NetApiBufferFree( ServerList );
            ServerList = NULL;

        }

        //
        //  Remote the api and pull the browse list from the remote server.
        //

        Status = RxNetServerEnum(RemoteMasterName,
                                     Network->NetworkName.Buffer,
                                     101,
                                     (LPBYTE *)&ServerList,
                                     0xffffffff,
                                     &EntriesRead,
                                     &TotalEntries,
                                     SV_TYPE_LOCAL_LIST_ONLY | SV_TYPE_DOMAIN_ENUM,
                                     NULL,
                                     NULL
                                     );

        if ((Status == NERR_Success) || (Status == ERROR_MORE_DATA)) {

            if (!LOCK_NETWORK(Network)) {
                try_return(NOTHING);
            }

            NetLocked = TRUE;

            Status = MergeServerList(&Network->DomainList,
                                     101,
                                     ServerList,
                                     EntriesRead,
                                     TotalEntries
                                     );
        }

try_exit:NOTHING;
    } finally {

        if (NetLocked) {
            UNLOCK_NETWORK(Network);
        }

        if ( NetReferenced ) {
            BrDereferenceNetwork( Network );
        }

        if (RemoteMasterName != NULL) {
            MIDL_user_free(RemoteMasterName);
        }

        MIDL_user_free(Context->RequestPacket);

        MIDL_user_free(Context);

        if ( ServerList != NULL ) {
            (void) NetApiBufferFree( ServerList );
        }

    }

    return;

}
