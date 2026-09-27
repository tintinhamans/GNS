//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_ICE_CLIENT_H
#define STEAMNETWORKINGSOCKETS_ICE_CLIENT_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include "../steamnetworkingsockets_thinker.h"
#include "steamnetworkingsockets_p2p_ice.h"
#include <memory>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

namespace SteamNetworkingSocketsLib {
    class CSteamNetworkingSocketsSTUNRequest;
    class CSteamNetworkingICESessionCallbacks;
    class CSteamNetworkingICESession;
    struct ICEServerEntry;
    struct ICEDNSResolveState;

    struct STUNHeader
    {
        uint32 m_nMessageType;
        uint32 m_nMessageLength;
        uint32 m_nTransactionID[3];
    };

    struct STUNAttribute
    {
        uint32 m_nType;
        uint32 m_nLength;
        const uint32 *m_pData;
    };

    /// Passed to RecvSTUNPacketCallback_t when a STUN request gets a reply
    struct RecvSTUNPktInfo_t
    {
        CSteamNetworkingSocketsSTUNRequest *m_pRequest;
		SteamNetworkingMicroseconds m_usecNow;
        const STUNHeader *m_pHeader;
        uint32 m_nAttributes;
        const STUNAttribute *m_pAttributes;
    };

    typedef void (CSteamNetworkingICESession::*RecvSTUNPacketCallback_t)( const RecvSTUNPktInfo_t &info );

    enum class ICECandidateKind
    {
        None,
        Host,
        ServerReflexive,
        Relayed,
        PeerReflexive,
    };

    /// Convert EICECandidateType, which is not family specific, to the more
    /// detailed and address-family-specific EICECandidateType.
    EICECandidateType CalcICECandidateType( ICECandidateKind kind, const netadr_t& addr );

    /// Represents one local network interface used for ICE candidate gathering.
    /// Owns its socket.  Server-reflexive and relay discovery each race a request against
    /// every configured server of this interface's address family concurrently; see
    /// m_vecPendingSrflxRequests / m_vecPendingAllocateRequests.
    struct ICESessionInterface
    {
        // The session that owns this interface.
        CSteamNetworkingICESession &m_session;

        // Local-preference component of the ICE priority formula
        // (RFC 8445 §5.1.2).  Assigned as a countdown from 65535 in
        // enumeration order so the first adapter returned by the OS gets
        // the highest preference.
        uint32 m_nPriority;

        // Subnet prefix length from the OS adapter enumeration (e.g. 24
        // for a /24 network).  Used to detect same-LAN peers.  0 if the
        // OS did not provide this information.
        int m_nPrefixLen;

        // Raw UDP socket bound to this interface for sending and receiving
        // ICE traffic.  Null only transiently during construction before
        // the socket is successfully opened.  Owned by this object.
        IRawUDPSocket *m_pSocket;

        // Cached copy of m_pSocket->m_boundAddr converted to netadr_t.
        // Set once when the socket is opened; use this rather than m_pSocket->m_boundAddr
        // to keep all internal address handling in one type.
        netadr_t m_boundAddr;

        // In-flight STUN request for keepalive, or TURN request for refresh/CreatePermission
        // maintenance on an already-won relay.  Null if none is active.  At most one per
        // interface -- these run one at a time against the single server we've already
        // committed to, unlike the discovery races below.
        CSteamNetworkingSocketsSTUNRequest *m_pPendingSTUNRequest = nullptr;

        // In-flight server-reflexive discovery requests, one per candidate STUN server of
        // this interface's address family, raced concurrently.  The first successful reply
        // wins (STUNRequestCallback_ServerReflexiveCandidate); the rest are cancelled.  Empty
        // once discovery is complete (m_addrSTUNServer valid and not failed) or exhausted.
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingSrflxRequests;

        // How many entries of the session's m_vecSTUNServers we have already dispatched a
        // discovery request for, from this interface.  Lets STUN servers resolved later
        // (e.g. a slow DNS lookup) join a still-running or already-exhausted race.
        int m_nSTUNServersDispatched = 0;

        // In-flight TURN Allocate discovery requests, one per candidate TURN server of this
        // interface's address family, raced concurrently with each other and with
        // m_vecPendingSrflxRequests.  The first successful allocation wins
        // (STUNRequestCallback_AllocateRelay); allocations that complete late on a loser are
        // released with a zero-lifetime Refresh instead of being left held.
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingAllocateRequests;

        // How many entries of the session's m_vecTURNServers we have already dispatched an
        // Allocate request for, from this interface.  See m_nSTUNServersDispatched.
        int m_nTURNServersDispatched = 0;

        // Server-reflexive discovery results.  m_addrSTUNServer is also used
        // as a "discovery complete" signal: it is all-zeros until a terminal
        // result (success, no-NAT, or timeout) has been recorded.
        netadr_t m_addrServerReflexive;  // invalid = not found / no-NAT
        netadr_t m_addrSTUNServer;       // server that gave us the result
        bool m_bServerReflexiveFailed = false;             // true if every dispatched STUN server timed out

        // Relay (TURN) discovery results.  m_addrTURNServer is also used
        // as a "discovery complete" signal: it is all-zeros until a terminal
        // result (success or timeout) has been recorded.
        netadr_t m_addrRelayed;     // invalid = no relay / not yet allocated
        netadr_t m_addrTURNServer;  // TURN server that gave us the relay
        bool m_bRelayFailed = false;                  // true if every dispatched TURN server timed out/failed

        // TURN long-term credentials (RFC 5766 section 10).  Populated on receipt of a
        // 401 challenge from the TURN server.  m_strTURNRealm is empty until then.
        // m_arrTURNKey holds MD5(username:realm:password), the HMAC-SHA1 key.
        std::string m_strTURNRealm;
        std::string m_strTURNNonce;
        uint8 m_arrTURNKey[16] = {};

        // When to send the next TURN Refresh.  Zero means no active allocation.
        // Set to now + lifetime/2 when an Allocate or Refresh response is received.
        SteamNetworkingMicroseconds m_usecRefreshAfter = 0;

        // Revision of the session's permitted-IP list that we last sent CreatePermission for.
        // When less than the session's m_nTURNPermissionRevision[v4/v6] for our family, a
        // new CreatePermission sweep is needed.
        int m_nTURNPermissionRevision = 0;

        /// Send a packet through this interface to the destination remote address.
        /// If relay address is non-zero, send via Send Indication to the TURN server
        bool SendPacketGather( int nChunks, const iovec *pChunks, int cbPayload, const netadr_t &addrPeer, const netadr_t &addrRelay );

        // Build and dispatch a local candidate discovery notification.
        // Computes the RFC 5245 candidate-attribute string and the family-specific
        // EICECandidateType, then calls m_session's OnLocalCandidateDiscovered callback.
        void NotifyLocalCandidateDiscovered( ICECandidateKind kind, const netadr_t& addr );

        // Send a STUN binding request for server-reflexive discovery, tracked in
        // m_vecPendingSrflxRequests so many servers can be raced concurrently.
        void QueueSrflxDiscoveryRequest( const netadr_t &addrSTUNServer, int nServerIdx, RecvSTUNPacketCallback_t cb, int nEncoding );

        // Send a STUN binding request for keepalive (single in-flight request per interface).
        void QueueBindRequest( const netadr_t &addrSTUNServer, RecvSTUNPacketCallback_t cb, int nEncoding );

        // Send (or re-send after an auth challenge) a TURN Allocate request for concurrent
        // relay discovery, tracked in m_vecPendingAllocateRequests.  pRealm/pNonce/pKey carry
        // long-term-credential state for this specific server if a prior challenge already
        // primed them; null for a first attempt.  nAuthRetriesUsed bounds the challenge/retry
        // loop against a misbehaving server.
        void QueueAllocateDiscoveryRequest( int nTURNServerIdx, const std::string *pRealm, const std::string *pNonce, const uint8 *pKey, int nAuthRetriesUsed = 0, bool bForceIPv4Relay = false );

        // Send a TURN Refresh request to keep the allocation alive.
        void QueueRefreshRequest( RecvSTUNPacketCallback_t cb, int nEncoding, int nAuthRetriesUsed = 0 );

        // Fire-and-forget release of a relay allocation this interface no longer needs (a
        // losing racer from concurrent Allocate discovery that still completed after another
        // server already won).  Sends a Refresh with LIFETIME=0; the response, if any, is
        // not tracked -- there is nothing more to do either way.
        void ReleaseTURNAllocation( const netadr_t &addrTURNServer, int nTURNServerIdx, const std::string &strRealm, const std::string &strNonce, const uint8 arrKey[16] );

        // Create and queue any TURN request, attaching long-term auth credentials when
        // we have them.  pExtraAttrs/nExtraAttrs are request-specific attrs (e.g.
        // REQUESTED-TRANSPORT for Allocate); auth attrs are appended after them.
        // Returns false and cleans up pRequestSlot if credential lookup fails.
        bool QueueTURNRequest( uint32 nMsgType, int nEncoding, const netadr_t &addrTURNServer, RecvSTUNPacketCallback_t cb, STUNAttribute *pExtraAttrs, int nExtraAttrs, CSteamNetworkingSocketsSTUNRequest *&pRequestSlot );

        ICESessionInterface( CSteamNetworkingICESession &session, uint32 nPriority, int nPrefixLen )
            : m_session( session ), m_nPriority( nPriority ), m_nPrefixLen( nPrefixLen ), m_pSocket( nullptr ) {}
        ~ICESessionInterface() { if ( m_pSocket ) m_pSocket->Close(); }

        ICESessionInterface( const ICESessionInterface& ) = delete;
        ICESessionInterface& operator=( const ICESessionInterface& ) = delete;
    };

    /// Identifies one local candidate: a socket (interface) plus an optional TURN relay.
    /// An interface with a relay allocation produces two local candidates -- one host
    /// (m_addrTURNServer all-zeros, send directly from the socket) and one relay
    /// (m_addrTURNServer non-zero, send via Send Indication to the TURN server).
    struct ICELocalCandidate
    {
        ICESessionInterface *m_pInterface;
        netadr_t m_addrTURNServer;  // invalid = host candidate
        bool IsRelay() const { return m_addrTURNServer.IsValid(); }

        // The address family this candidate actually sends/receives on.  For a host candidate
        // that's the bound interface address; for a relay candidate it's the TURN-allocated
        // relayed address, which is not guaranteed to match the interface's own family (e.g. a
        // TURN server that ignores REQUESTED-ADDRESS-FAMILY and always relays over IPv4).
        // RFC 8445 pairing must be done strictly within one address family, so this -- not the
        // interface's bound address -- is what candidate-pair formation should compare against.
        ipadrtype_t GetAddressType() const
        {
            return IsRelay() ? m_pInterface->m_addrRelayed.GetType() : m_pInterface->m_boundAddr.GetType();
        }
    };

    // Parsed representation of an RFC 5245 candidate-attribute line.
    // https://datatracker.ietf.org/doc/html/rfc5245#section-15.1
    struct RFC5245CandidateAttr {
        std::string sFoundation;
        int nComponent;
        std::string sTransport;
        int nPriority;
        netadr_t address;
        std::string sType;
        ICECandidateKind nType;
        std_vector< std::pair< std::string, std::string > > vAttrs;
    };

    const uint32 k_nSTUN_MaxPacketSize_Bytes = 576; //  RFC 5389 7.1
    // RFC 5389 sec 15.3: the USERNAME attribute value MUST be less than 513 bytes.  (RFC 8489
    // sec 14.3 tightens this to fewer than 509; we use the more permissive 5389 limit so we
    // still accept any peer conforming to either.)  We reject anything longer on ingestion.
    const uint32 k_nSTUN_MaxUsernameLen_Bytes = 512;
    const uint32 k_nSTUN_CookieValue = 0x2112A442;
    const uint32 k_nSTUN_BindingRequest = 0x0001;
    const uint32 k_nSTUN_BindingResponse = 0x0101;
    const uint32 k_nSTUN_BindingErrorResponse = 0x0111;
    const uint32 k_nSTUN_Attr_MappedAddress = 0x0001;
    const uint32 k_nSTUN_Attr_UserName = 0x0006;
    const uint32 k_nSTUN_Attr_MessageIntegrity = 0x0008;
    const uint32 k_nSTUN_Attr_ErrorCode = 0x0009;
    const uint32 k_nSTUN_Attr_Realm = 0x0014;
    const uint32 k_nSTUN_Attr_Nonce = 0x0015;
    const uint32 k_nSTUN_Attr_MessageIntegrity_SHA256 = 0x001C;
    const uint32 k_nSTUN_Attr_XORMappedAddress = 0x0020;
    const uint32 k_nSTUN_Attr_Priority = 0x0024;
    const uint32 k_nSTUN_Attr_UseCandidate = 0x0025;
    const uint32 k_nSTUN_Attr_Fingerprint = 0x8028;
    const uint32 k_nSTUN_Attr_ICEControlled = 0x8029;
    const uint32 k_nSTUN_Attr_ICEControlling = 0x802A;

    // TURN message types (RFC 5766)
    const uint32 k_nTURN_AllocateRequest         = 0x0003;
    const uint32 k_nTURN_AllocateSuccess         = 0x0103;
    const uint32 k_nTURN_AllocateError           = 0x0113;
    const uint32 k_nTURN_RefreshRequest          = 0x0004;
    const uint32 k_nTURN_RefreshSuccess          = 0x0104;
    const uint32 k_nTURN_CreatePermissionRequest = 0x0008;
    const uint32 k_nTURN_CreatePermissionSuccess = 0x0108;
    const uint32 k_nTURN_SendIndication          = 0x0016;
    const uint32 k_nTURN_DataIndication          = 0x0017;

    // TURN attribute types (RFC 5766)
    const uint32 k_nTURN_Attr_Lifetime           = 0x000D;
    const uint32 k_nTURN_Attr_XORPeerAddress     = 0x0012;
    const uint32 k_nTURN_Attr_Data               = 0x0013;
    const uint32 k_nTURN_Attr_RequestedAddressFamily = 0x0017; // RFC 8656 sec 18.5
    const uint32 k_nTURN_Attr_XORRelayedAddress  = 0x0016;
    const uint32 k_nTURN_Attr_RequestedTransport = 0x0019;

    // STUN/TURN error codes we act on (RFC 5389 sec 15.6, RFC 5766 sec 10, RFC 8656 sec 18.6).
    const int k_nTURNErrorCode_Unauthorized          = 401; // no (or wrong) long-term credentials
    const int k_nTURNErrorCode_StaleNonce            = 438; // credentials were fine, but the nonce expired
    const int k_nTURNErrorCode_UnknownAttribute      = 420; // server doesn't understand REQUESTED-ADDRESS-FAMILY
    const int k_nTURNErrorCode_AddressFamilyNotSupported = 440; // server understands it, but won't allocate IPv6 here

    enum STUNPacketEncodingFlags
    {
        kSTUNPacketEncodingFlags_None = 0,
        kSTUNPacketEncodingFlags_NoFingerprint = 1,  // Do not emit a fingerprint attr
        kSTUNPacketEncodingFlags_MappedAddress = 2,  // Use MappedAddress, not XORMappedAddress
        kSTUNPacketEncodingFlags_NoMappedAddress = 4, // Do not emit *any* address attribute at all.
        kSTUNPacketEncodingFlags_MessageIntegrity = 8, // Use MessageIntegrity, not MessageIntegrity_SHA256
    };

	/// Track an in-flight STUN request.  The thinker interface is used to handle
	/// retry and timeout.  Note, that there is no list of in-flight requests,
	/// we use the thinker system to extant requests.  All read and write access
	/// to these objects require holding the global lock
    class CSteamNetworkingSocketsSTUNRequest final : private IThinker
    {
    public:
        explicit CSteamNetworkingSocketsSTUNRequest( ICESessionInterface *pInterface );
        ~CSteamNetworkingSocketsSTUNRequest();

        // The local interface this request was sent from.  Set at construction, never null.
        ICESessionInterface * const m_pInterface;
        uint32 m_nTransactionID[3];  // generated at construction
        netadr_t m_remoteAddr; // Address of the peer
        netadr_t m_addrRelay;
        uint32 m_nMessageType;
        int m_nRetryCount;
        int m_nMaxRetries;
        RecvSTUNPacketCallback_t m_callback = nullptr;
        std::string m_strPassword;
		SteamNetworkingMicroseconds m_usecLastSentTime;

        // For TURN CreatePermission requests, the revision number of our permissions list in this request.
        // Not used for other request types
        int m_nTURNPermissionRevision;

        // For STUN binding and TURN Allocate requests, the index of the server in the
        // session's STUN or TURN server list.  Used to fail over to the next entry.
        int m_nServerIdx = 0;

        // For TURN Allocate discovery requests only: the long-term-credential state this
        // specific request used (if any), captured from that server's 401/438 challenge.
        // Carried onto the winning interface's persistent fields on success so
        // Think_TURNMaintenance can keep authenticating to the same server afterward.
        std::string m_strTURNRealm;
        std::string m_strTURNNonce;
        uint8 m_arrTURNKey[16] = {};

        // How many times this logical TURN operation has already been retried after an
        // auth challenge (401 or 438).  Carried forward into each retry's replacement
        // request and capped to stop a misbehaving server from looping us forever.
        int m_nAuthRetriesUsed = 0;

        // For TURN Allocate requests only: true if this specific attempt asked for an IPv6
        // relay (RFC 8656 REQUESTED-ADDRESS-FAMILY).  Lets the response handler tell a genuine
        // "unknown attribute" (420) or "family not supported" (440) apart from an unrelated
        // error, so it knows to retry once without the attribute instead of giving up.
        bool m_bRequestedIPv6Relay = false;

        // Serialize the packet and start the retry loop.
        void Queue( uint32 nMessageType, int nEncoding, netadr_t remoteAddr, RecvSTUNPacketCallback_t cb, STUNAttribute *pExtraAttrs = nullptr, int nExtraAttrs = 0 );

        // Immediately retransmit and reset the exponential backoff schedule, as if
        // the request were freshly queued.  The transaction ID is preserved, so any
        // response already in flight will still be matched and processed.
        void RetriggerNow();

        // Handle an incoming STUN reply that has already been matched to this request
        // by transaction ID.  Verifies message integrity, fires the callback, then
        // deletes this request.
        void ReplyPacketReceived( const RecvPktInfo_t &info, const STUNHeader &header );

    private:
        // Implements IThinker
        virtual void Think( SteamNetworkingMicroseconds usecNow ) override;

        // Serialized packet built by Queue(); Think() retransmits verbatim.
        uint32 m_packet[ k_nSTUN_MaxPacketSize_Bytes/4 ];
        int m_cbPacketSize;
    };

	/// Main logic of establishing an ICE session with a peer.  In real-world
	/// uses cases this is always associated one-to-one with a CConnectionTransportP2PICE_Valve.
	/// But breaking it out into a separate object helps with testing.
	/// Also, this object is only protected by the global lock, and accessing
	/// the transport also requires the connection lock.
    class CSteamNetworkingICESession : private IThinker
    {
    public:
        CSteamNetworkingICESession( EICERole role, CSteamNetworkingICESessionCallbacks *pCallbacks, int nEncoding );
		CSteamNetworkingICESession( const ICESessionConfig& cfg, CSteamNetworkingICESessionCallbacks *pCallbacks );
		~CSteamNetworkingICESession();

        void StartSession();
        void InvalidateInterfaceList();

        struct ICECandidateBase
        {
            ICECandidateKind m_type;
            netadr_t m_addr;
            uint32 m_nPriority;
            ICECandidateBase();
            ICECandidateBase( ICECandidateKind t, const netadr_t& addr );
        };
        EICERole GetRole() { return m_role; }
        EICECandidateType AddPeerCandidate( const RFC5245CandidateAttr& attr );
        void SetRemoteUsername( const char *pszUsername );
		void SetRemotePassword( const char *pszPassword );

        const char* GetLocalPassword() { return m_strLocalPassword.c_str(); }
        bool BCanSendEndToEnd() const { return m_pSelectedCandidatePair != nullptr; }
		int GetPing() const;

        // Returns true if the address is on the same subnet as ANY of our local LAN/localhost
        // adapters, not just one specific interface.  Used to classify a route as "probably local".
        bool BIsAddressOnAnyLocalSubnet( const netadr_t &addr ) const;

        bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal );

    protected:
        void Think( SteamNetworkingMicroseconds usecNow ) override;

        friend struct ICESessionInterface;

    private:
        struct ICEPeerCandidate : public ICECandidateBase
        {
            std::string m_sFoundation;
            ICEPeerCandidate( const ICECandidateBase& c, const char *pszFoundation ) : ICECandidateBase( c ), m_sFoundation( pszFoundation ) {}
        };

        enum ICECandidatePairState
        {
            kICECandidatePairState_Frozen,
            kICECandidatePairState_Waiting,
            kICECandidatePairState_InProgress,
            kICECandidatePairState_Succeeded,
            kICECandidatePairState_Failed,
            kICECandidatePairState_None

        };
        struct ICECandidatePair
        {
            ICECandidatePairState m_nState;
            bool m_bNominated;
            // True if this pair's local and remote endpoints are on the same LAN/localhost
            // subnet -- i.e. a direct link.  Cached at construction; used to prefer such pairs
            // during exploration/nomination (BIsPreferredRouteOver).
            bool m_bLocalSubnet;
            uint64 m_nPriority;
            ICELocalCandidate m_localCandidate;
            ICEPeerCandidate m_remoteCandidate;
            CSteamNetworkingSocketsSTUNRequest *m_pPeerRequest;
			int m_nLastRecordedPing;
            ICECandidatePair( const ICELocalCandidate& localCandidate, const ICEPeerCandidate& remoteCandidate, EICERole role );

            // Route-selection preference: prefer a same-subnet (direct LAN) pair, then higher
            // RFC 8445 pair priority.  Used for both check ordering and nomination.  This is a
            // local policy only; the priorities we put on the wire stay RFC-standard.
            bool BIsPreferredRouteOver( const ICECandidatePair &x ) const
            {
                if ( m_bLocalSubnet != x.m_bLocalSubnet )
                    return m_bLocalSubnet;
                return m_nPriority > x.m_nPriority;
            }
        };

        CSteamNetworkingICESessionCallbacks *m_pCallbacks;

        // ICE defines two asymmetric roles: the controlling agent drives candidate
        // nomination (picks which path wins), the controlled agent follows its lead.
        // Roles are assigned by the signaling layer before ICE starts and stay fixed
        // unless a role-conflict packet forces a swap.
        EICERole m_role;

        // Random 64-bit value generated fresh each session, included in every
        // outgoing connectivity check.  When both sides accidentally claim the same
        // role, whichever has the numerically larger tiebreaker keeps that role and
        // the other side switches.
        uint64 m_nRoleTiebreaker;

        // Set to true whenever the local network topology might have changed (e.g.
        // at startup, or on a network-change notification).  The next Think() pass
        // will re-enumerate interfaces and rebuild candidates, then clear this flag.
        bool m_bInterfaceListStale;

        // Bitmask of kSTUNPacketEncodingFlags_* controlling STUN wire format quirks:
        // whether to include a fingerprint, whether to use legacy MappedAddress vs
        // XOR-MappedAddress, and whether to sign with HMAC-SHA1 (MessageIntegrity)
        // vs HMAC-SHA256.  Set to MessageIntegrity at construction for compatibility
        // with peers that don't support SHA256.
        int m_nEncoding;

        // Our ICE credentials for this session, generated once at startup from the
        // connection ID and a random block.  The username fragment identifies us;
        // the password is the HMAC key used to sign and verify connectivity checks.
        std::string m_strLocalUsernameFragment;
        std::string m_strLocalPassword;

        // The remote peer's ICE credentials, delivered via signaling.  Empty until
        // the first auth message arrives; connectivity checks cannot be validated
        // or sent until both are populated.
        std::string m_strRemoteUsernameFragment;
        std::string m_strRemotePassword;

        // Pre-built USERNAME attribute strings derived from the two ufrag values above.
        // ICE mandates the format "recipient:sender" -- so the direction reverses
        // depending on whether a packet is inbound or outbound.  Caching them avoids
        // repeated string concatenation in the hot path.
        // Both are empty until remote credentials have arrived via signaling.
        std::string m_strIncomingUsername;  // local:remote -- expected in packets we receive
        std::string m_strOutgoingUsername;  // remote:local -- placed in packets we send

        // Dirty flag set whenever a local or remote candidate is added or removed.
        // The next Think() pass rebuilds m_vecCandidatePairs from the current
        // cross-product, then clears this flag.
        bool m_bCandidatePairsNeedUpdate;

        // Bitmask of k_EICECandidate_* types the caller has authorized us to gather
        // and use.  Host-only vs reflexive vs relay, IPv4 vs IPv6, are all controlled
        // here.  A candidate whose type bit is absent is silently dropped during
        // gathering rather than advertised to the peer.
        int m_nPermittedCandidateTypes;

        // Timestamp of the next keepalive to send on the selected path.  Zero means
        // "send immediately on the next Think()."  Only meaningful once
        // m_pSelectedCandidatePair is non-null.
        SteamNetworkingMicroseconds m_nextKeepalive;

        // The candidate pair currently in use for sending and receiving application
        // data.  Null until ICE nominates a winner.  Once set, changes only if the
        // selected path fails and a new one is nominated.
        // m_pSelectedSocket is the pre-looked-up raw socket for the local candidate
        // in that pair, kept here to avoid the per-send lookup overhead.
        ICECandidatePair *m_pSelectedCandidatePair;

        // Local network interfaces discovered during the most recent enumeration.
        // Each entry represents one usable local address.  Rebuilt whenever
        // m_bInterfaceListStale is set.
        std_vector< std::unique_ptr<ICESessionInterface> > m_vecInterfaces;

        // Resolved addresses of STUN servers, populated once at construction from the
        // config string.  Used to discover server-reflexive candidates and to dispatch
        // STUN responses back to the correct server.
        std_vector< netadr_t > m_vecSTUNServers;

        // Resolved addresses of TURN servers, populated once at construction from the
        // config.  Used to allocate relay candidates.
        std_vector< netadr_t > m_vecTURNServers;

        // Per-server TURN long-term credentials, parallel to m_vecTURNServers.
        // Empty strings = no auth required for that server.
        struct TURNCredentials { std::string m_strUsername; std::string m_strPassword; };
        std_vector< TURNCredentials > m_vecTURNCredentials;

        // True if any configured TURN server is on a LAN/private IP.  This doesn't
        // really happen in production environments, it's only in weird test situations
        bool m_bAnyTURNServerLANAddress = false;

        std::shared_ptr<ICEDNSResolveState> m_pDNSResolveState;
        void AddServers( std_vector< ICEServerEntry > &vecEntries );
        void Think_ApplyPendingDNSResults();

        // De-duplicated lists of peer IP addresses (port zeroed) that we should
        // ask each relay to permit forwarding from.  LAN/loopback/link-local
        // addresses are excluded.  Updated whenever AddPeerCandidate() adds a
        // new public IP for a family; the corresponding revision counter is
        // incremented so relay interfaces can detect the need to re-send
        // CreatePermission.  IPv4 and IPv6 are tracked separately because each
        // relay interface only needs permissions for its own address family.
        std_vector<CIPAddress> m_vecTURNPermittedIPv4;
        std_vector<CIPAddress> m_vecTURNPermittedIPv6;
        int m_nTURNPermissionRevisionIPv4 = 0;
        int m_nTURNPermissionRevisionIPv6 = 0;

        // Candidates received from the remote peer via signaling.  Paired with
        // m_vecInterfaces to form m_vecCandidatePairs.
        std_vector< ICEPeerCandidate > m_vecPeerCandidates;

        // In-flight STUN Binding requests being used as ICE connectivity checks,
        // one per candidate pair currently under active test.
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingPeerRequests;

        // All formed candidate pairs (cross-product of local × remote candidates,
        // pruned for duplicates).  Checked in priority order during Think(); each
        // pair tracks its own state (waiting, in-progress, succeeded, failed).
        std_vector< ICECandidatePair* > m_vecCandidatePairs;

        // Pairs needing an immediate out-of-turn connectivity check, bypassing the
        // normal paced schedule.  Used when the controlling agent nominates a pair
        // (one final check with USE-CANDIDATE must be sent) or when an incoming check
        // reveals a new valid pair that should be verified promptly.  Drained LIFO on
        // each Think() pass before the regular check list.
        std_vector< ICECandidatePair* > m_vecTriggeredCheckQueue;

        // Timestamp of session start, or of the most recently added/updated peer candidate,
        // whichever is later.  Think_CheckAllCandidatesFailed waits a short grace period past
        // this before concluding nothing new is coming, so a slow signaling round-trip can't
        // be mistaken for exhaustion.
        SteamNetworkingMicroseconds m_usecLastPeerCandidateActivity = 0;

        // Set once OnAllCandidatesFailed has fired for this session, so it only fires once.
        bool m_bAllCandidatesFailedNotified = false;

        void GatherInterfaces();
        void UpdateKeepalive( ICESessionInterface *pIntf );

        void Think_KeepAliveOnCandidates( SteamNetworkingMicroseconds usecNow );
        void Think_DiscoverServerReflexiveCandidates();
        void Think_DiscoverRelayCandidate();
        void Think_TURNMaintenance( SteamNetworkingMicroseconds usecNow );
        void Think_TestPeerConnectivity();

        // True once gathering has reached a terminal state (success or exhaustion) on every
        // interface for both candidate types, no DNS resolution is still pending, and every
        // formed candidate pair is Failed (none Succeeded/InProgress/Waiting/Frozen) with no
        // peer-candidate activity for at least k_usecAllCandidatesFailedGracePeriod.  Never
        // true while gathering or checks could still plausibly produce a working pair.
        bool BAllCandidatesExhausted( SteamNetworkingMicroseconds usecNow ) const;
        void Think_CheckAllCandidatesFailed( SteamNetworkingMicroseconds usecNow );

        void SetSelectedCandidatePair( ICECandidatePair *pPair );

        // Delete a candidate pair and perform all associated cleanup:
        // clears the selected-pair state if this was the active path, cancels
        // any in-flight peer connectivity check, and removes the pair from the
        // triggered-check queue.  Does NOT remove it from m_vecCandidatePairs --
        // that is the caller's responsibility.
        void InternalDeleteCandidatePair( ICECandidatePair *pPair );

        void STUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info );
        void STUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info );
        void STUNRequestCallback_AllocateRelay( const RecvSTUNPktInfo_t &info );
        void STUNRequestCallback_RefreshAllocation( const RecvSTUNPktInfo_t &info );
        void STUNRequestCallback_CreatePermission( const RecvSTUNPktInfo_t &info );
        void STUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info );

        void OnPacketReceived( const RecvPktInfo_t &info, ICESessionInterface *pInterface, netadr_t *pAddrRelay = nullptr );
        static void StaticPacketReceived( const RecvPktInfo_t &info, ICESessionInterface *pContext );
    };

    class CSteamNetworkingICESessionCallbacks
    {
    public:
        virtual void OnLocalCandidateDiscovered( EICECandidateType type, const char *pszCandidateStr ) {}
        virtual void OnPacketReceived( const RecvPktInfo_t &info ) {}
        virtual void OnConnectionSelected( const ICELocalCandidate& localCandidate, const CSteamNetworkingICESession::ICECandidateBase& remoteCandidate ) {}

        // Called at most once per session: every local and remote candidate has been tried
        // (gathering finished on all interfaces, no DNS resolution still pending) and every
        // resulting candidate pair failed.  There is no point waiting out the rest of the
        // connect timeout; the caller should end the connection now.
        virtual void OnAllCandidatesFailed() {}
    };


	/// Connection transport that sends datagrams using the route discovered
	/// by our ICE client, CSteamNetworkingICESession
    class CConnectionTransportP2PICE_Valve final
        : public CConnectionTransportP2PICE, public CSteamNetworkingICESessionCallbacks
    {
    public:
        CConnectionTransportP2PICE_Valve( CSteamNetworkConnectionP2P &connection );
    	void Init( const ICESessionConfig& cfg );

    private:
        CSteamNetworkingICESession *m_pICESession;

    	virtual bool BCanSendEndToEndData() const override;
		virtual void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow ) override;
    	virtual void TransportFreeResources() override;

        // Implements CConnectionTransportUDPBase
        virtual bool SendPacket( const void *pkt, int cbPkt ) override;
        virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;

    protected:
        virtual void OnLocalCandidateDiscovered( EICECandidateType type, const char *pszCandidateStr ) override;
        virtual void OnPacketReceived( const RecvPktInfo_t &info ) override;
        virtual void OnConnectionSelected( const ICELocalCandidate& localCandidate, const CSteamNetworkingICESession::ICECandidateBase& remoteCandidate ) override;
        virtual void OnAllCandidatesFailed() override;
    };

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // _H
