
#include "hadooprpc.h"
#include "varint.h"

#include "proto/IpcConnectionContext.pb-c.h"
#include "proto/ProtobufRpcEngine.pb-c.h"
#include "proto/ClientNamenodeProtocol.pb-c.h"
#include "proto/RpcHeader.pb-c.h"
#include "proto/datatransfer.pb-c.h"

#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static
void * hadoop_namenode_worker(void * p)
{
  Hadoop__Hdfs__RenewLeaseRequestProto request = HADOOP__HDFS__RENEW_LEASE_REQUEST_PROTO__INIT;
  Hadoop__Hdfs__RenewLeaseResponseProto * response = NULL;
  int res;
  struct connection_state * state = (struct connection_state *) p;
  request.clientname = (char *) state->clientname;

  while(true)
  {
    // "If the client is not actively using the lease, it will time out after one minute (default value)."
    // http://itm-vm.shidler.hawaii.edu/HDFS/ArchDocDecomposition.html
    sleep(30);

    res = hadoop_rpc_call_namenode(
      state,
      &hadoop__hdfs__client_namenode_protocol__descriptor,
      "renewLease",
      (const ProtobufCMessage *) &request,
      (ProtobufCMessage **) &response);
    if(res == 0)
    {
      hadoop__hdfs__renew_lease_response_proto__free_unpacked(response, NULL);
    }
  }

  return NULL;
}

static inline
ssize_t hadoop_rpc_send(const struct connection_state * state, const void * const it, const ssize_t len)
{
  return sendto(state->sockfd, it, len, 0, (struct sockaddr *) &state->servaddr, sizeof(state->servaddr));
}

static inline
ssize_t hadoop_rpc_send_int32(const struct connection_state * state, const uint32_t it)
{
  uint32_t it_in_n = htonl(it);
  return sendto(state->sockfd, &it_in_n, sizeof(it_in_n), 0, (struct sockaddr *) &state->servaddr, sizeof(state->servaddr));
}

#define CONNECTION_CONTEXT_CALL_ID -3

#define PACK(len, buf, msg) \
  do \
  { \
    uint8_t lenbuf[10]; \
    uint8_t lenlen; \
    len = protobuf_c_message_get_packed_size((const ProtobufCMessage *) msg); \
    lenlen = encode_unsigned_varint(lenbuf, len); \
    buf = alloca(lenlen + len); \
    memcpy(buf, lenbuf, lenlen); \
    protobuf_c_message_pack((const ProtobufCMessage *) msg, buf + lenlen); \
    len += lenlen; \
  } while(0)

int
hadoop_rpc_call_namenode(
  struct connection_state * state,
  const ProtobufCServiceDescriptor * service,
  const char * methodname,
  const ProtobufCMessage * in,
  ProtobufCMessage ** out)
{
  int res;
  void * headerbuf;
  uint32_t headerlen;
  void * requestbuf;
  uint32_t requestlen;
  Hadoop__Common__RpcRequestHeaderProto header = HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__INIT;
  Hadoop__Common__RequestHeaderProto request = HADOOP__COMMON__REQUEST_HEADER_PROTO__INIT;
  uint32_t responselen;
  void * responsebuf;
  uint32_t responseheaderlen;
  uint8_t lenlen;
  Hadoop__Common__RpcResponseHeaderProto * response;
  void * msgbuf;
  uint32_t msglen;
  const ProtobufCMethodDescriptor * method = protobuf_c_service_descriptor_get_method_by_name(service, methodname);

  pthread_mutex_lock(&state->mutex);

  PACK(msglen, msgbuf, in);

  header.rpckind = HADOOP__COMMON__RPC_KIND_PROTO__RPC_PROTOCOL_BUFFER;
  header.has_rpckind = true;
  header.rpcop = HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__OPERATION_PROTO__RPC_FINAL_PACKET;
  header.has_rpcop = true;
  header.callid = state->next_call_id++;
  PACK(headerlen, headerbuf, &header);

  request.declaringclassprotocolname = "org.apache.hadoop.hdfs.protocol.ClientProtocol";
  request.clientprotocolversion = 1;
  request.methodname = (char *) methodname;
  PACK(requestlen, requestbuf, &request);

  hadoop_rpc_send_int32(state, headerlen + requestlen + msglen);
  hadoop_rpc_send(state, headerbuf, headerlen);
  hadoop_rpc_send(state, requestbuf, requestlen);
  hadoop_rpc_send(state, msgbuf, msglen);

  res = recvfrom(state->sockfd, &responselen, sizeof(responselen), MSG_WAITALL, NULL, NULL);
  if(res < 0)
  {
    goto cleanup;
  }
  responselen = ntohl(responselen);
  responsebuf = alloca(responselen);
  res = recvfrom(state->sockfd, responsebuf, responselen, MSG_WAITALL, NULL, NULL);
  if(res < 0)
  {
    goto cleanup;
  }

  responseheaderlen = decode_unsigned_varint(responsebuf, &lenlen);
  responsebuf += lenlen;
  response = hadoop__common__rpc_response_header_proto__unpack(NULL, responseheaderlen, responsebuf);
  responsebuf += responseheaderlen;

  switch(response->status)
  {
  case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_STATUS_PROTO__SUCCESS:
    hadoop__common__rpc_response_header_proto__free_unpacked(response, NULL);

    responseheaderlen = decode_unsigned_varint(responsebuf, &lenlen);
    responsebuf += lenlen;
    *out = protobuf_c_message_unpack(method->output, NULL, responseheaderlen, responsebuf);

    res = 0;
    goto cleanup;
  case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_STATUS_PROTO__FATAL:
    hadoop_rpc_disconnect(state);
  // fall-through
  case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_STATUS_PROTO__ERROR:
  default:

    if(response->has_errordetail)
    {
      switch(response->errordetail)
      {
      case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_ERROR_CODE_PROTO__FATAL_UNAUTHORIZED:
        res = -EACCES;
        break;
      case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_ERROR_CODE_PROTO__FATAL_VERSION_MISMATCH:
      case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_ERROR_CODE_PROTO__ERROR_RPC_VERSION_MISMATCH:
        res = -ERPCMISMATCH;
        break;
      case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_ERROR_CODE_PROTO__FATAL_INVALID_RPC_HEADER:
      case HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_ERROR_CODE_PROTO__ERROR_RPC_SERVER:
        res = -EBADRPC;
        break;
      default:
        res = -EINVAL;
        break;
      }
    }
    else
    {
      res = -EINVAL;
    }

    hadoop__common__rpc_response_header_proto__free_unpacked(response, NULL);
    goto cleanup;
  }

cleanup:
  pthread_mutex_unlock(&state->mutex);
  return res;
}

int
hadoop_rpc_disconnect(struct connection_state * state)
{
  close(state->sockfd); // don't care if we fail
  state->isconnected = false;
  pthread_cancel(state->worker); // don't care if we fail
  return 0;
}

int
hadoop_rpc_connect_namenode(struct connection_state * state, const char * host, const uint16_t port)
{
  int error;
  uint8_t header[] = { 'h', 'r', 'p', 'c', 9, 0, 0};
  Hadoop__Common__IpcConnectionContextProto context = HADOOP__COMMON__IPC_CONNECTION_CONTEXT_PROTO__INIT;
  Hadoop__Common__UserInformationProto userinfo = HADOOP__COMMON__USER_INFORMATION_PROTO__INIT;
  void * contextbuf;
  uint32_t contextlen;
  void * rpcheaderbuf;
  uint32_t rpcheaderlen;
  Hadoop__Common__RpcRequestHeaderProto rpcheader = HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__INIT;

  pthread_mutex_lock(&state->mutex);

  state->sockfd = socket(AF_INET, SOCK_STREAM, 0);
  state->servaddr.sin_family = AF_INET;
  state->servaddr.sin_addr.s_addr = inet_addr(host);
  state->servaddr.sin_port = htons(port);

  error = connect(state->sockfd, (struct sockaddr *) &state->servaddr, sizeof(state->servaddr));
  if(error < 0)
  {
    goto fail;
  }

  error = hadoop_rpc_send(state, &header, sizeof(header));
  if(error < 0)
  {
    goto fail;
  }

  userinfo.effectiveuser = getenv("USER");
  context.userinfo = &userinfo;
  context.protocol = "org.apache.hadoop.hdfs.protocol.ClientProtocol";
  PACK(contextlen, contextbuf, &context);

  rpcheader.rpckind = HADOOP__COMMON__RPC_KIND_PROTO__RPC_PROTOCOL_BUFFER;
  rpcheader.has_rpckind = true;
  rpcheader.rpcop = HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__OPERATION_PROTO__RPC_FINAL_PACKET;
  rpcheader.has_rpcop = true;
  rpcheader.callid = CONNECTION_CONTEXT_CALL_ID;
  PACK(rpcheaderlen, rpcheaderbuf, &rpcheader);

  error = hadoop_rpc_send_int32(state, rpcheaderlen + contextlen);
  if(error < 0)
  {
    goto fail;
  }
  error = hadoop_rpc_send(state, rpcheaderbuf, rpcheaderlen);
  if(error < 0)
  {
    goto fail;
  }
  error = hadoop_rpc_send(state, contextbuf, contextlen);
  if(error < 0)
  {
    goto fail;
  }

  error = pthread_create(&state->worker, NULL, hadoop_namenode_worker, state);
  if(error < 0)
  {
    goto fail;
  }

  state->isconnected = true;
  pthread_mutex_unlock(&state->mutex);
  return 0;

fail:
  pthread_mutex_unlock(&state->mutex);
  hadoop_rpc_disconnect(state);
  return error;
}

int
hadoop_rpc_connect_datanode(struct connection_state * state, const char * host, const uint16_t port)
{
  int res;

  state->sockfd = socket(AF_INET, SOCK_STREAM, 0);
  state->servaddr.sin_family = AF_INET;
  state->servaddr.sin_addr.s_addr = inet_addr(host);
  state->servaddr.sin_port = htons(port);

  res = connect(state->sockfd, (struct sockaddr *) &state->servaddr, sizeof(state->servaddr));
  if(res < 0)
  {
    return -EHOSTUNREACH;
  }
  else
  {
    state->isconnected = true;
    return 0;
  }
}

int hadoop_rpc_call_datanode(
  struct connection_state * state,
  uint8_t type,
  const ProtobufCMessage * in,
  Hadoop__Hdfs__BlockOpResponseProto ** out)
{
  int res;
  void * buf;
  uint32_t len;
  Hadoop__Hdfs__BlockOpResponseProto * response;
  uint8_t responselen_varint[5];
  uint8_t responselen_varintlen;
  uint64_t responselen;
  void * responsebuf;
  uint8_t bytesinwrongbuf;
  uint8_t header[] = { 0, 28, type };

  res = hadoop_rpc_send(state, &header, sizeof(header));
  if(res < 0)
  {
    return -EPROTO;
  }
  PACK(len, buf, in);
  res = hadoop_rpc_send(state, buf, len);
  if(res < 0)
  {
    return -EPROTO;
  }

  res = recvfrom(state->sockfd, &responselen_varint[0], sizeof(responselen_varint), MSG_WAITALL, NULL, NULL);
  if(res < 0)
  {
    return res;
  }
  responselen = decode_unsigned_varint(&responselen_varint[0], &responselen_varintlen);
  bytesinwrongbuf = 5 - responselen_varintlen;
  responsebuf = alloca(responselen);
  if(bytesinwrongbuf > 0)
  {
    // copy remaining bytes from buffer
    memcpy(responsebuf, responselen_varint + responselen_varintlen, bytesinwrongbuf);
  }
  res = recvfrom(state->sockfd, responsebuf + bytesinwrongbuf, responselen - bytesinwrongbuf, MSG_WAITALL, NULL, NULL);
  if(res < 0)
  {
    return res;
  }
  *out = response = hadoop__hdfs__block_op_response_proto__unpack(NULL, responselen, responsebuf);
  switch(response->status)
  {
  case HADOOP__HDFS__STATUS__ERROR:
    res = -EINVAL;
    break;
  case HADOOP__HDFS__STATUS__ERROR_CHECKSUM:
    res = -EIO;
    break;
  case HADOOP__HDFS__STATUS__ERROR_INVALID:
    res = -EINVAL;
    break;
  case HADOOP__HDFS__STATUS__ERROR_EXISTS:
    res = -EEXIST;
    break;
  case HADOOP__HDFS__STATUS__ERROR_ACCESS_TOKEN:
    res = -EACCES;
    break;
  case HADOOP__HDFS__STATUS__ERROR_UNSUPPORTED:
    res = -ENOSYS;
    break;
  case HADOOP__HDFS__STATUS__SUCCESS:
  case HADOOP__HDFS__STATUS__CHECKSUM_OK:
    res = 0;
    break;
  default:
    res = -ENOTSUP;
    break;
  }

  return res;
}

int hadoop_rpc_receive_packets(
  struct connection_state * state,
  uint8_t * to)
{
  int res;
  uint32_t packetlen;
  uint16_t headerlen;
  bool more = true;
  Hadoop__Hdfs__ClientReadStatusProto ack = HADOOP__HDFS__CLIENT_READ_STATUS_PROTO__INIT;
  void * buf;
  uint32_t len;

  while(more)
  {
    void * headerbuf;
    Hadoop__Hdfs__PacketHeaderProto * header;

    res = recvfrom(state->sockfd, &packetlen, sizeof(packetlen), MSG_WAITALL, NULL, NULL);
    if(res < 0)
    {
      return res;
    }
    packetlen = ntohl(packetlen);

    res = recvfrom(state->sockfd, &headerlen, sizeof(headerlen), MSG_WAITALL, NULL, NULL);
    if(res < 0)
    {
      return res;
    }
    headerlen = ntohs(headerlen);

    headerbuf = alloca(headerlen);
    res = recvfrom(state->sockfd, headerbuf, headerlen, MSG_WAITALL, NULL, NULL);
    if(res < 0)
    {
      return res;
    }
    header = hadoop__hdfs__packet_header_proto__unpack(NULL, headerlen, headerbuf);

    more = !header->lastpacketinblock;
    res = recvfrom(state->sockfd, to, header->datalen, MSG_WAITALL, NULL, NULL);
    hadoop__hdfs__packet_header_proto__free_unpacked(header, NULL);

    if(res < 0)
    {
      return res;
    }
  }

  // ack transfer
  ack.status = HADOOP__HDFS__STATUS__SUCCESS;
  PACK(len, buf, &ack);
  res = hadoop_rpc_send(state, buf, len);
  if(res < 0)
  {
    return res;
  }

  return 0;
}

