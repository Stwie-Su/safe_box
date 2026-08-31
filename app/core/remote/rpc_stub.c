/**
 * @file rpc_stub.c
 * RPC 空实现：远程通道未启用时编译本文件，接口签名与正式实现完全一致。
 */

#include "core/remote/rpc.h"

#include <stdio.h>

void rpc_init(const char * host, int port)
{
    (void)host; (void)port;
    printf("[rpc] 远程通道未启用，指令分发停用\n");
}

void rpc_poll(void)
{
}

void rpc_publish_status_now(void)
{
}

void rpc_publish_event(const char * evt, const char * user, const char * detail, int res)
{
    (void)evt; (void)user; (void)detail; (void)res;
}

bool rpc_take_local_otp(char * req_id, size_t cap)
{
    (void)req_id; (void)cap;
    return false;
}

void rpc_set_event_hook(rpc_event_hook_t hook)
{
    (void)hook;
}
