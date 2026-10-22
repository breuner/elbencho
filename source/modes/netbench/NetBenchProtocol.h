// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHPROTOCOL_H_
#define MODES_NETBENCH_NETBENCHPROTOCOL_H_

#include <cstdint>

/*
 * Constants shared by the netbench client and server side: timeouts, the tagged message layout
 * and the JSON keys of the connect request/reply and the interface query over the service HTTP
 * API.
 */

#define NETBENCH_CONNECT_TIMEOUT_SEC    60 // max time for a client to wait for its server's prep
#define NETBENCH_RECEIVE_TIMEOUT_SEC    20 // max time without progress on a stream
#define NETBENCH_WAIT_TIMEOUT_MS        500 // completion wait slice, then interruption check
#define NETBENCH_CONNECT_RETRY_MS       500 // delay between connect attempts
#define NETBENCH_QUERY_TIMEOUT_SEC      10 // coordinator: interface query of a service
#define NETBENCH_CM_STEP_TIMEOUT_MS     10000 // rdmacm: each of address/route resolution and connect
#define NETBENCH_SPIN_USEC              10 // non-blocking completion polling before blocking wait
#define NETBENCH_DRAIN_QUIET_MS         100 // server endpoint drain ends after this without messages
#define NETBENCH_DRAIN_MAX_MS           5000 // max duration of a server endpoint drain
#define NETBENCH_CQ_BATCH_SIZE          64 // completions to read per call
#define NETBENCH_PING_LEN               8 // ping message length
#define NETBENCH_TCP_HELLO_MAGIC        0x454C4E42u // "ELNB": first word of the tcp engine's hello
#define NETBENCH_CM_MAGIC               0x454C4243u // "ELBC": first word of the rdmacm private data
#define NETBENCH_MIN_TAG_BITS           36 // tag bits used by the layout below (libfabric)

// libfabric tag layout: bits 0..31 stream (global client thread rank), bits 32..35 message type
#define NETBENCH_TAG_RANK_MASK          0xFFFFFFFFULL
#define NETBENCH_TAG_TYPE_SHIFT         32
#define NETBENCH_TAG_TYPE_MASK          (0xFULL << NETBENCH_TAG_TYPE_SHIFT)

enum NetBenchMsgType
{
    NetBenchMsg_DATA = 0, // block from client or response from server
    NetBenchMsg_PING = 1, // connection check from client after connect
};

inline uint64_t netBenchMakeTag(NetBenchMsgType type, uint32_t rank)
    { return ( (uint64_t)type << NETBENCH_TAG_TYPE_SHIFT) | rank; }
inline NetBenchMsgType netBenchTagType(uint64_t tag)
    { return (NetBenchMsgType)( (tag & NETBENCH_TAG_TYPE_MASK) >> NETBENCH_TAG_TYPE_SHIFT); }
inline uint32_t netBenchTagRank(uint64_t tag)
    { return (uint32_t)(tag & NETBENCH_TAG_RANK_MASK); }

// JSON keys of the "/netbenchconnect" request and reply (engine-specific ones are only used by
// the named engine)
#define NETBENCH_JSON_RUNID             "runid"
#define NETBENCH_JSON_CLIENTRANK        "clientrank"
#define NETBENCH_JSON_ENGINE            "engine" // engine name, must match on both sides
#define NETBENCH_JSON_ENDPOINT          "endpoint" // human-readable endpoint description
#define NETBENCH_JSON_ADDRFORMAT        "addrformat" // endpoint address format
#define NETBENCH_JSON_ADDR              "addr" // endpoint address (hex)
#define NETBENCH_JSON_PROVIDER          "provider" // libfabric
#define NETBENCH_JSON_PROTOCOL          "protocol" // libfabric
#define NETBENCH_JSON_PROTOCOLVERSION   "protocolversion" // libfabric
#define NETBENCH_JSON_LIBVERSION        "libversion" // libfabric runtime version
#define NETBENCH_JSON_IBV_LID           "ibvlid" // ibverbs: port LID
#define NETBENCH_JSON_IBV_GID           "ibvgid" // ibverbs: GID (hex)
#define NETBENCH_JSON_IBV_GIDTYPE       "ibvgidtype" // ibverbs: 0 IB, 1 RoCE v1, 2 RoCE v2
#define NETBENCH_JSON_IBV_QPN           "ibvqpn" // ibverbs: queue pair number
#define NETBENCH_JSON_IBV_PSN           "ibvpsn" // ibverbs: initial packet sequence number
#define NETBENCH_JSON_IBV_MTU           "ibvmtu" // ibverbs: active MTU (enum ibv_mtu)

// the "/netbenchifaces" query parameter and reply keys (interfaces of a service for the placement)
#define NETBENCH_QUERY_HOST             "host" // the service host as the coordinator addresses it
#define NETBENCH_QUERY_DEVSUBNETS       "devsubnets" // "1": subnets from the "--netdevs" entries
#define NETBENCH_JSON_IFACES            "ifaces" // interfaces for the client role
#define NETBENCH_JSON_SERVERIFACES      "serverifaces" // interfaces for the server role
#define NETBENCH_JSON_SUBNET            "subnet" // "network/prefix" of an interface address

#endif /* MODES_NETBENCH_NETBENCHPROTOCOL_H_ */
