/**
 * bitcoind.js
 * Copyright (c) 2014, BitPay (MIT License)
 *
 * bitcoindjs.cc:
 *   A bitcoind node.js binding.
 */

#include "nan.h"

#include "bitcoindjs.h"

/**
 * Bitcoin headers
 */

#if defined(HAVE_CONFIG_H)
#include "bitcoin-config.h"
#endif

#include "core.h"
#include "addrman.h"
#include "checkpoints.h"
#include "crypter.h"
#include "main.h"
// #include "random.h"
// #include "timedata.h"

#ifdef ENABLE_WALLET
#include "db.h"
#include "wallet.h"
#include "walletdb.h"
#endif

// #include "walletdb.h"
#include "alert.h"
#include "checkqueue.h"
// #include "db.h"
#include "miner.h"
#include "rpcclient.h"
#include "tinyformat.h"
// #include "wallet.h"
#include "allocators.h"
#include "clientversion.h"
#include "hash.h"
#include "mruset.h"
#include "rpcprotocol.h"
#include "txdb.h"
#include "base58.h"
#include "coincontrol.h"
#include "init.h"
#include "netbase.h"
#include "rpcserver.h"
#include "txmempool.h"
#include "bloom.h"
#include "coins.h"
#include "key.h"
#include "net.h"
#include "script.h"
#include "ui_interface.h"
// #include "chainparamsbase.h"
#include "compat.h"
#include "keystore.h"
#include "noui.h"
#include "serialize.h"
#include "uint256.h"
#include "chainparams.h"
#include "core.h"
#include "leveldbwrapper.h"
// #include "pow.h"
#include "sync.h"
#include "util.h"
// #include "chainparamsseeds.h"
// #include "core_io.h"
#include "limitedmap.h"
#include "protocol.h"
#include "threadsafety.h"
#include "version.h"

/**
 * Bitcoin Globals
 * Relevant:
 *  ~/bitcoin/src/init.cpp
 *  ~/bitcoin/src/bitcoind.cpp
 *  ~/bitcoin/src/main.h
 */

#include <stdint.h>
#include <signal.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <openssl/crypto.h>

using namespace std;
using namespace boost;

extern void DetectShutdownThread(boost::thread_group*);
extern int nScriptCheckThreads;
extern bool fDaemon;
extern std::map<std::string, std::string> mapArgs;
#ifdef ENABLE_WALLET
extern std::string strWalletFile;
extern CWallet *pwalletMain;
#endif

/**
 * Node and Templates
 */

#include <node.h>
#include <string>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

using namespace node;
using namespace v8;

NAN_METHOD(StartBitcoind);
NAN_METHOD(IsStopping);
NAN_METHOD(IsStopped);
NAN_METHOD(StopBitcoind);
NAN_METHOD(GetBlock);
NAN_METHOD(GetTx);
NAN_METHOD(PollBlocks);
NAN_METHOD(PollMempool);
NAN_METHOD(BroadcastTx);
NAN_METHOD(VerifyBlock);
NAN_METHOD(VerifyTransaction);

NAN_METHOD(WalletNewAddress);
NAN_METHOD(GetAccountAddress);
NAN_METHOD(SetAccount);
NAN_METHOD(GetAccount);
NAN_METHOD(SendToAddress);
NAN_METHOD(SignMessage);
NAN_METHOD(VerifyMessage);
NAN_METHOD(GetBalance);
NAN_METHOD(GetUnconfirmedBalance);
NAN_METHOD(SendFrom);
NAN_METHOD(ListTransactions);
NAN_METHOD(ListAccounts);
NAN_METHOD(GetTransaction);
NAN_METHOD(BackupWallet);
NAN_METHOD(WalletPassphrase);
NAN_METHOD(WalletPassphraseChange);
NAN_METHOD(WalletLock);
NAN_METHOD(EncryptWallet);
NAN_METHOD(SetTxFee);

static void
async_start_node_work(uv_work_t *req);

static void
async_start_node_after(uv_work_t *req);

static void
async_stop_node_work(uv_work_t *req);

static void
async_stop_node_after(uv_work_t *req);

static int
start_node(void);

static void
start_node_thread(void);

static void
async_get_block(uv_work_t *req);

static void
async_get_block_after(uv_work_t *req);

static void
async_get_tx(uv_work_t *req);

static void
async_get_tx_after(uv_work_t *req);

static void
async_poll_blocks(uv_work_t *req);

static void
async_poll_blocks_after(uv_work_t *req);

static void
async_poll_mempool(uv_work_t *req);

static void
async_poll_mempool_after(uv_work_t *req);

static void
async_broadcast_tx(uv_work_t *req);

static void
async_broadcast_tx_after(uv_work_t *req);

static inline void
ctx_to_js(const CTransaction& tx, uint256 hashBlock, Local<Object> entry);

static inline void
cblock_to_js(const CBlock& block, const CBlockIndex* blockindex, Local<Object> obj);

extern "C" void
init(Handle<Object>);

/**
 * Private Variables
 */

static volatile bool shutdownComplete = false;

/**
 * async_node_data
 * Where the uv async request data resides.
 */

struct async_node_data {
  char *err_msg;
  char *result;
  Persistent<Function> callback;
};

/**
 * async_log_data
 * Where the uv async request data resides.
 */

struct async_log_data {
  char *err_msg;
  int **out_pipe;
  int **log_pipe;
  char *result;
  Persistent<Function> callback;
};

/**
 * async_block_data
 */

struct async_block_data {
  std::string err_msg;
  std::string hash;
  CBlock result_block;
  CBlockIndex* result_blockindex;
  Persistent<Function> callback;
};

/**
 * async_tx_data
 */

struct async_tx_data {
  std::string err_msg;
  std::string txHash;
  std::string blockHash;
  CTransaction result_tx;
  Persistent<Function> callback;
};

/**
 * async_poll_blocks_data
 */

struct async_poll_blocks_data {
  std::string err_msg;
  int poll_saved_height;
  int poll_top_height;
  Persistent<Array> result_array;
  Persistent<Function> callback;
};

/**
 * async_poll_mempool_data
 */

struct async_poll_mempool_data {
  std::string err_msg;
  int poll_saved_height;
  int poll_top_height;
  Persistent<Array> result_array;
  Persistent<Function> callback;
};

/**
 * async_broadcast_tx_data
 */

struct async_broadcast_tx_data {
  std::string err_msg;
  std::string tx_hex;
  std::string tx_hash;
  bool override_fees;
  bool own_only;
  Persistent<Function> callback;
};

/**
 * StartBitcoind
 * bitcoind.start(callback)
 */

NAN_METHOD(StartBitcoind) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoind.start(callback)");
  }

  Local<Function> callback = Local<Function>::Cast(args[0]);

  //
  // Run bitcoind's StartNode() on a separate thread.
  //

  async_node_data *data_start_node = new async_node_data();
  data_start_node->err_msg = NULL;
  data_start_node->result = NULL;
  data_start_node->callback = Persistent<Function>::New(callback);

  uv_work_t *req_start_node = new uv_work_t();
  req_start_node->data = data_start_node;

  int status_start_node = uv_queue_work(uv_default_loop(),
    req_start_node, async_start_node_work,
    (uv_after_work_cb)async_start_node_after);

  assert(status_start_node == 0);

  NanReturnValue(NanNew<Number>(-1));
}

/**
 * async_start_node_work()
 * Call start_node() and start all our boost threads.
 */

static void
async_start_node_work(uv_work_t *req) {
  async_node_data *node_data = static_cast<async_node_data*>(req->data);
  start_node();
  while (!pwalletMain) {
    useconds_t usec = 100 * 1000;
    usleep(usec);
  }
  node_data->result = (char *)strdup("start_node(): bitcoind opened.");
}

/**
 * async_start_node_after()
 * Execute our callback.
 */

static void
async_start_node_after(uv_work_t *req) {
  NanScope();
  async_node_data *node_data = static_cast<async_node_data*>(req->data);

  if (node_data->err_msg != NULL) {
    Local<Value> err = Exception::Error(String::New(node_data->err_msg));
    free(node_data->err_msg);
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    node_data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(String::New(node_data->result))
    };
    TryCatch try_catch;
    node_data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  // node_data->callback.Dispose();

  if (node_data->result != NULL) {
    free(node_data->result);
  }

  delete node_data;
  delete req;
}

/**
 * IsStopping()
 * bitcoind.stopping()
 */

NAN_METHOD(IsStopping) {
  NanScope();
  NanReturnValue(NanNew<Boolean>(ShutdownRequested()));
}

/**
 * IsStopped()
 * bitcoind.stopped()
 */

NAN_METHOD(IsStopped) {
  NanScope();
  NanReturnValue(NanNew<Boolean>(shutdownComplete));
}

/**
 * start_node(void)
 * start_node_thread(void)
 * A reimplementation of AppInit2 minus
 * the logging and argument parsing.
 */

static int
start_node(void) {
  noui_connect();

  (boost::thread *)new boost::thread(boost::bind(&start_node_thread));

  // horrible fix for a race condition
  sleep(2);
  signal(SIGINT, SIG_DFL);
  signal(SIGHUP, SIG_DFL);

  return 0;
}

static void
start_node_thread(void) {
  boost::thread_group threadGroup;
  boost::thread *detectShutdownThread = NULL;

  const int argc = 0;
  const char *argv[argc + 1] = {
    //"-server",
    NULL
  };
  ParseParameters(argc, argv);
  ReadConfigFile(mapArgs, mapMultiArgs);
  if (!SelectParamsFromCommandLine()) {
    return;
  }
  // CreatePidFile(GetPidFile(), getpid());
  detectShutdownThread = new boost::thread(
    boost::bind(&DetectShutdownThread, &threadGroup));

  int fRet = AppInit2(threadGroup);

  if (!fRet) {
    if (detectShutdownThread)
      detectShutdownThread->interrupt();
    threadGroup.interrupt_all();
  }

  if (detectShutdownThread) {
    detectShutdownThread->join();
    delete detectShutdownThread;
    detectShutdownThread = NULL;
  }
  Shutdown();
  shutdownComplete = true;
}

/**
 * StopBitcoind
 * bitcoind.stop(callback)
 */

NAN_METHOD(StopBitcoind) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoind.stop(callback)");
  }

  Local<Function> callback = Local<Function>::Cast(args[0]);

  //
  // Run bitcoind's StartShutdown() on a separate thread.
  //

  async_node_data *data_stop_node = new async_node_data();
  data_stop_node->err_msg = NULL;
  data_stop_node->result = NULL;
  data_stop_node->callback = Persistent<Function>::New(callback);

  uv_work_t *req_stop_node = new uv_work_t();
  req_stop_node->data = data_stop_node;

  int status_stop_node = uv_queue_work(uv_default_loop(),
    req_stop_node, async_stop_node_work,
    (uv_after_work_cb)async_stop_node_after);

  assert(status_stop_node == 0);

  NanReturnValue(Undefined());
}

/**
 * async_stop_node_work()
 * Call StartShutdown() to join the boost threads, which will call Shutdown().
 */

static void
async_stop_node_work(uv_work_t *req) {
  async_node_data *node_data = static_cast<async_node_data*>(req->data);
  StartShutdown();
  node_data->result = (char *)strdup("stop_node(): bitcoind shutdown.");
}

/**
 * async_stop_node_after()
 * Execute our callback.
 */

static void
async_stop_node_after(uv_work_t *req) {
  NanScope();
  async_node_data* node_data = static_cast<async_node_data*>(req->data);

  if (node_data->err_msg != NULL) {
    Local<Value> err = Exception::Error(String::New(node_data->err_msg));
    free(node_data->err_msg);
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    node_data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(String::New(node_data->result))
    };
    TryCatch try_catch;
    node_data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  node_data->callback.Dispose();

  if (node_data->result != NULL) {
    free(node_data->result);
  }

  delete node_data;
  delete req;
}

/**
 * GetBlock
 * bitcoind.getBlock(blockHash, callback)
 */

NAN_METHOD(GetBlock) {
  NanScope();

  if (args.Length() < 2
      || !args[0]->IsString()
      || !args[1]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoindjs.getBlock(blockHash, callback)");
  }

  String::Utf8Value hash(args[0]->ToString());
  Local<Function> callback = Local<Function>::Cast(args[1]);

  std::string hashp = std::string(*hash);

  async_block_data *data = new async_block_data();
  data->err_msg = std::string("");
  data->hash = hashp;
  data->callback = Persistent<Function>::New(callback);

  uv_work_t *req = new uv_work_t();
  req->data = data;

  int status = uv_queue_work(uv_default_loop(),
    req, async_get_block,
    (uv_after_work_cb)async_get_block_after);

  assert(status == 0);

  NanReturnValue(Undefined());
}

static void
async_get_block(uv_work_t *req) {
  async_block_data* data = static_cast<async_block_data*>(req->data);
  std::string strHash = data->hash;
  if (strHash[1] != 'x') {
    strHash = "0x" + strHash;
  }
  uint256 hash(strHash);
  CBlock block;
  CBlockIndex* pblockindex = mapBlockIndex[hash];
  if (ReadBlockFromDisk(block, pblockindex)) {
    data->result_block = block;
    data->result_blockindex = pblockindex;
  } else {
    data->err_msg = std::string("get_block(): failed.");
  }
}

static void
async_get_block_after(uv_work_t *req) {
  NanScope();
  async_block_data* data = static_cast<async_block_data*>(req->data);

  if (!data->err_msg.empty()) {
    Local<Value> err = Exception::Error(String::New(data->err_msg.c_str()));
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    const CBlock& block = data->result_block;
    const CBlockIndex* blockindex = data->result_blockindex;

    Local<Object> obj = NanNew<Object>();
    cblock_to_js(block, blockindex, obj);

    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(obj)
    };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  data->callback.Dispose();

  delete data;
  delete req;
}

/**
 * GetTx
 * bitcoind.getTx(txHash, [blockHash], callback)
 */

NAN_METHOD(GetTx) {
  NanScope();

  if (args.Length() < 3
      || !args[0]->IsString()
      || !args[1]->IsString()
      || !args[2]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoindjs.getTx(txHash, [blockHash], callback)");
  }

  String::Utf8Value txHash_(args[0]->ToString());
  String::Utf8Value blockHash_(args[1]->ToString());
  Local<Function> callback = Local<Function>::Cast(args[2]);

  Persistent<Function> cb;
  cb = Persistent<Function>::New(callback);

  std::string txHash = std::string(*txHash_);
  std::string blockHash = std::string(*blockHash_);

  if (blockHash.empty()) {
    blockHash = std::string("0x0000000000000000000000000000000000000000000000000000000000000000");
  }

  if (txHash[1] != 'x') {
    txHash = "0x" + txHash;
  }

  if (blockHash[1] != 'x') {
    blockHash = "0x" + blockHash;
  }

  async_tx_data *data = new async_tx_data();
  data->err_msg = std::string("");
  data->txHash = txHash;
  data->blockHash = blockHash;
  data->callback = Persistent<Function>::New(callback);

  uv_work_t *req = new uv_work_t();
  req->data = data;

  int status = uv_queue_work(uv_default_loop(),
    req, async_get_tx,
    (uv_after_work_cb)async_get_tx_after);

  assert(status == 0);

  NanReturnValue(Undefined());
}

static void
async_get_tx(uv_work_t *req) {
  async_tx_data* data = static_cast<async_tx_data*>(req->data);

  uint256 hash(data->txHash);
  uint256 hashBlock(data->blockHash);
  CTransaction tx;

  if (GetTransaction(hash, tx, hashBlock, true)) {
    data->result_tx = tx;
  } else {
    data->err_msg = std::string("get_tx(): failed.");
  }
}

static void
async_get_tx_after(uv_work_t *req) {
  NanScope();
  async_tx_data* data = static_cast<async_tx_data*>(req->data);

  std::string txHash = data->txHash;
  std::string blockHash = data->blockHash;
  CTransaction tx = data->result_tx;

  uint256 hash(txHash);
  uint256 hashBlock(blockHash);

  if (!data->err_msg.empty()) {
    Local<Value> err = Exception::Error(String::New(data->err_msg.c_str()));
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;
    string strHex = HexStr(ssTx.begin(), ssTx.end());

    Local<Object> entry = NanNew<Object>();
    entry->Set(NanNew<String>("hex"), NanNew<String>(strHex));
    ctx_to_js(tx, hashBlock, entry);

    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(entry)
    };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  data->callback.Dispose();

  delete data;
  delete req;
}

/**
 * PollBlocks
 * bitcoind.pollBlocks(callback)
 */

NAN_METHOD(PollBlocks) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoindjs.pollBlocks(callback)");
  }

  Local<Function> callback = Local<Function>::Cast(args[0]);

  async_poll_blocks_data *data = new async_poll_blocks_data();
  data->poll_saved_height = -1;
  data->poll_top_height = -1;
  data->err_msg = std::string("");
  data->callback = Persistent<Function>::New(callback);

  uv_work_t *req = new uv_work_t();
  req->data = data;

  int status = uv_queue_work(uv_default_loop(),
    req, async_poll_blocks,
    (uv_after_work_cb)async_poll_blocks_after);

  assert(status == 0);

  NanReturnValue(Undefined());
}

static void
async_poll_blocks(uv_work_t *req) {
  async_poll_blocks_data* data = static_cast<async_poll_blocks_data*>(req->data);

  data->poll_saved_height = data->poll_top_height;

  while (chainActive.Tip()) {
    int cur_height = chainActive.Height();
    if (cur_height != data->poll_top_height) {
      data->poll_top_height = cur_height;
      break;
    } else {
      // 100 milliseconds
      useconds_t usec = 100 * 1000;
      usleep(usec);
    }
  }
}

static void
async_poll_blocks_after(uv_work_t *req) {
  NanScope();
  async_poll_blocks_data* data = static_cast<async_poll_blocks_data*>(req->data);

  if (!data->err_msg.empty()) {
    Local<Value> err = Exception::Error(String::New(data->err_msg.c_str()));
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    const unsigned argc = 2;
    Local<Array> blocks = NanNew<Array>();

    for (int i = data->poll_saved_height, j = 0; i < data->poll_top_height; i++) {
      if (i == -1) continue;
      CBlockIndex *pindex = chainActive[i];
      if (pindex != NULL) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindex)) {
          Local<Object> obj = NanNew<Object>();
          cblock_to_js(block, pindex, obj);
          blocks->Set(j, obj);
          j++;
        }
      }
    }

    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(blocks)
    };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  data->callback.Dispose();

  delete data;
  delete req;
}

/**
 * PollMempool
 * bitcoind.pollMempool(callback)
 */

NAN_METHOD(PollMempool) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoindjs.pollMempool(callback)");
  }

  Local<Function> callback = Local<Function>::Cast(args[0]);

  async_poll_mempool_data *data = new async_poll_mempool_data();
  data->poll_saved_height = -1;
  data->poll_top_height = -1;
  data->err_msg = std::string("");
  data->callback = Persistent<Function>::New(callback);

  uv_work_t *req = new uv_work_t();
  req->data = data;

  int status = uv_queue_work(uv_default_loop(),
    req, async_poll_mempool,
    (uv_after_work_cb)async_poll_mempool_after);

  assert(status == 0);

  NanReturnValue(Undefined());
}

static void
async_poll_mempool(uv_work_t *req) {
  // async_poll_blocks_data* data = static_cast<async_poll_blocks_data*>(req->data);
  // Nothing really async to do here. It's all in memory. Placeholder for now.
  useconds_t usec = 20 * 1000;
  usleep(usec);
}

static void
async_poll_mempool_after(uv_work_t *req) {
  NanScope();
  async_poll_blocks_data* data = static_cast<async_poll_blocks_data*>(req->data);

  if (!data->err_msg.empty()) {
    Local<Value> err = Exception::Error(String::New(data->err_msg.c_str()));
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    int ti = 0;
    Local<Array> txs = NanNew<Array>();

    {
      std::map<uint256, CTxMemPoolEntry>::const_iterator it = mempool.mapTx.begin();
      for (; it != mempool.mapTx.end(); it++) {
        const CTransaction& tx = it->second.GetTx();
        Local<Object> entry = NanNew<Object>();
        ctx_to_js(tx, 0, entry);
        txs->Set(ti, entry);
        ti++;
      }
    }

    {
      std::map<COutPoint, CInPoint>::const_iterator it = mempool.mapNextTx.begin();
      for (; it != mempool.mapNextTx.end(); it++) {
        const CTransaction tx = *it->second.ptx;
        Local<Object> entry = NanNew<Object>();
        ctx_to_js(tx, 0, entry);
        txs->Set(ti, entry);
        ti++;
      }
    }

    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(txs)
    };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  data->callback.Dispose();

  delete data;
  delete req;
}

/**
 * BroadcastTx
 * bitcoind.broadcastTx(tx, override_fees, own_only, callback)
 */

NAN_METHOD(BroadcastTx) {
  NanScope();

  if (args.Length() < 4
      || !args[0]->IsObject()
      || !args[1]->IsBoolean()
      || !args[2]->IsBoolean()
      || !args[3]->IsFunction()) {
    return NanThrowError(
      "Usage: bitcoindjs.broadcastTx(tx, override_fees, own_only, callback)");
  }

  Local<Object> js_tx = Local<Object>::Cast(args[0]);
  Local<Function> callback = Local<Function>::Cast(args[3]);

  String::Utf8Value tx_hex_(js_tx->Get(NanNew<String>("hex"))->ToString());
  std::string tx_hex = std::string(*tx_hex_);

  async_broadcast_tx_data *data = new async_broadcast_tx_data();
  data->tx_hex = tx_hex;
  data->override_fees = args[1]->ToBoolean()->IsTrue();
  data->own_only = args[2]->ToBoolean()->IsTrue();
  data->err_msg = std::string("");
  data->callback = Persistent<Function>::New(callback);

  uv_work_t *req = new uv_work_t();
  req->data = data;

  int status = uv_queue_work(uv_default_loop(),
    req, async_broadcast_tx,
    (uv_after_work_cb)async_broadcast_tx_after);

  assert(status == 0);

  NanReturnValue(Undefined());
}

static void
async_broadcast_tx(uv_work_t *req) {
  async_broadcast_tx_data* data = static_cast<async_broadcast_tx_data*>(req->data);

  CDataStream ssData(ParseHex(data->tx_hex), SER_NETWORK, PROTOCOL_VERSION);
  CTransaction tx;

  bool fOverrideFees = false;
  bool fOwnOnly = false;

  if (data->override_fees) {
    fOverrideFees = true;
  }

  if (data->own_only) {
    fOwnOnly = true;
  }

  try {
    ssData >> tx;
  } catch (std::exception &e) {
    data->err_msg = std::string("TX decode failed");
    return;
  }

  uint256 hashTx = tx.GetHash();

  bool fHave = false;
  CCoinsViewCache &view = *pcoinsTip;
  CCoins existingCoins;
  if (fOwnOnly) {
    fHave = view.GetCoins(hashTx, existingCoins);
    if (!fHave) {
      CValidationState state;
      if (!AcceptToMemoryPool(mempool, state, tx, false, NULL, !fOverrideFees)) {
        data->err_msg = std::string("TX rejected");
        return;
      }
    }
  }

  if (fHave) {
    if (existingCoins.nHeight < 1000000000) {
      data->err_msg = std::string("transaction already in block chain");
      return;
    }
  } else {
    SyncWithWallets(hashTx, tx, NULL);
  }

  RelayTransaction(tx, hashTx);

  data->tx_hash = hashTx.GetHex();
}

static void
async_broadcast_tx_after(uv_work_t *req) {
  NanScope();
  async_broadcast_tx_data* data = static_cast<async_broadcast_tx_data*>(req->data);

  if (!data->err_msg.empty()) {
    Local<Value> err = Exception::Error(String::New(data->err_msg.c_str()));
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  } else {
    CDataStream ssData(ParseHex(data->tx_hex), SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    ssData >> tx;
    Local<Object> entry = NanNew<Object>();
    ctx_to_js(tx, 0, entry);

    const unsigned argc = 3;
    Local<Value> argv[argc] = {
      Local<Value>::New(Null()),
      Local<Value>::New(NanNew<String>(data->tx_hash)),
      Local<Value>::New(entry)
    };
    TryCatch try_catch;
    data->callback->Call(Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(try_catch);
    }
  }

  data->callback.Dispose();

  delete data;
  delete req;
}

/**
 * VerifyBlock
 */

NAN_METHOD(VerifyBlock) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.verifyBlock(block)");
  }

  Local<Object> js_block = Local<Object>::Cast(args[0]);

  String::Utf8Value block_hex_(js_block->Get(NanNew<String>("hex"))->ToString());
  std::string block_hex = std::string(*block_hex_);

  CBlock block;
  CDataStream ssData(ParseHex(block_hex), SER_NETWORK, PROTOCOL_VERSION);
  ssData >> block;

  CValidationState state;
  bool valid = CheckBlock(block, state);

  NanReturnValue(NanNew<Boolean>(valid));
}

/**
 * VerifyTransaction
 */

NAN_METHOD(VerifyTransaction) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.verifyTransaction(tx)");
  }

  Local<Object> js_tx = Local<Object>::Cast(args[0]);

  String::Utf8Value tx_hex_(js_tx->Get(NanNew<String>("hex"))->ToString());
  std::string tx_hex = std::string(*tx_hex_);

  CTransaction tx;
  CDataStream ssData(ParseHex(tx_hex), SER_NETWORK, PROTOCOL_VERSION);
  ssData >> tx;

  CValidationState state;
  bool valid = CheckTransaction(tx, state);

  string reason;
  bool standard = IsStandardTx(tx, reason);

  NanReturnValue(NanNew<Boolean>(valid && standard));
}

/**
 * Wallet
 */

int64_t
GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth) {
  int64_t nBalance = 0;

  // Tally wallet transactions
  for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
      it != pwalletMain->mapWallet.end(); ++it) {
    const CWalletTx& wtx = (*it).second;
    if (!IsFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0) {
      continue;
    }

    int64_t nReceived, nSent, nFee;
    wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee);

    if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
      nBalance += nReceived;
    }
    nBalance -= nSent + nFee;
  }

  // Tally internal accounting entries
  nBalance += walletdb.GetAccountCreditDebit(strAccount);

  return nBalance;
}

int64_t
GetAccountBalance(const string& strAccount, int nMinDepth) {
  CWalletDB walletdb(pwalletMain->strWalletFile);
  return GetAccountBalance(walletdb, strAccount, nMinDepth);
}

NAN_METHOD(WalletNewAddress) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.walletNewAddress(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  if (!pwalletMain->IsLocked()) {
    pwalletMain->TopUpKeyPool();
  }

  // Generate a new key that is added to wallet
  CPubKey newKey;

  if (!pwalletMain->GetKeyFromPool(newKey)) {
    // return NanThrowError("Keypool ran out, please call keypoolrefill first");
    // EnsureWalletIsUnlocked();
    if (pwalletMain->IsLocked()) {
      return NanThrowError("Please enter the wallet passphrase with walletpassphrase first.");
    }
    pwalletMain->TopUpKeyPool(100);
    if (pwalletMain->GetKeyPoolSize() < 100) {
      return NanThrowError("Error refreshing keypool.");
    }
  }

  CKeyID keyID = newKey.GetID();

  pwalletMain->SetAddressBook(keyID, strAccount, "receive");

  NanReturnValue(NanNew<String>(CBitcoinAddress(keyID).ToString()));
}

NAN_METHOD(GetAccountAddress) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.getAccountAddress(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(SetAccount) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.setAccount(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(GetAccount) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.getAccount(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(SendToAddress) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.sendToAddress(options)");
  }

  Local<Object> options = Local<Object>::Cast(args[0]);

  String::Utf8Value addr_(options->Get(NanNew<String>("address"))->ToString());
  std::string addr = std::string(*addr_);

  CBitcoinAddress address(addr);

  if (!address.IsValid()) {
    return NanThrowError("Invalid Bitcoin address");
  }

  // Amount
  int64_t nAmount = options->Get(NanNew<String>("amount"))->IntegerValue();

  // Wallet comments
  CWalletTx wtx;
  if (options->Get(NanNew<String>("comment"))->IsString()) {
    String::Utf8Value comment_(options->Get(NanNew<String>("comment"))->ToString());
    std::string comment = std::string(*comment_);
    wtx.mapValue["comment"] = comment;
  }
  if (options->Get(NanNew<String>("to"))->IsString()) {
    String::Utf8Value to_(options->Get(NanNew<String>("to"))->ToString());
    std::string to = std::string(*to_);
    wtx.mapValue["to"] = to;
  }

  // EnsureWalletIsUnlocked();
  if (pwalletMain->IsLocked()) {
    return NanThrowError("Please enter the wallet passphrase with walletpassphrase first.");
  }

  string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
  if (strError != "") {
    return NanThrowError(strError.c_str());
  }

  std::string tx_hash = wtx.GetHash().GetHex();

  NanReturnValue(NanNew<String>(tx_hash));
}

NAN_METHOD(SignMessage) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.signMessage(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(VerifyMessage) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.verifyMessage(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(GetBalance) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.getBalance(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(GetUnconfirmedBalance) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.getUnconfirmedBalance(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(SendFrom) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.sendFrom(options)");
  }

  Local<Object> options = Local<Object>::Cast(args[0]);

  String::Utf8Value from_(options->Get(NanNew<String>("from"))->ToString());
  std::string from = std::string(*from_);
  String::Utf8Value addr_(options->Get(NanNew<String>("address"))->ToString());
  std::string addr = std::string(*addr_);

  string strAccount = from;
  CBitcoinAddress address(addr);

  if (!address.IsValid()) {
    return NanThrowError("Invalid Bitcoin address");
  }

  int64_t nAmount = options->Get(NanNew<String>("amount"))->IntegerValue();
  int nMinDepth = 1;
  if (options->Get(NanNew<String>("minDepth"))->IsNumber()) {
    nMinDepth = options->Get(NanNew<String>("minDepth"))->IntegerValue();
  }

  CWalletTx wtx;
  wtx.strFromAccount = strAccount;
  if (options->Get(NanNew<String>("comment"))->IsString()) {
    String::Utf8Value comment_(options->Get(NanNew<String>("comment"))->ToString());
    std::string comment = std::string(*comment_);
    wtx.mapValue["comment"] = comment;
  }
  if (options->Get(NanNew<String>("to"))->IsString()) {
    String::Utf8Value to_(options->Get(NanNew<String>("to"))->ToString());
    std::string to = std::string(*to_);
    wtx.mapValue["to"] = to;
  }

  // EnsureWalletIsUnlocked();
  if (pwalletMain->IsLocked()) {
    return NanThrowError("Please enter the wallet passphrase with walletpassphrase first.");
  }

  // Check funds
  int64_t nBalance = GetAccountBalance(strAccount, nMinDepth);
  if (nAmount > nBalance) {
    return NanThrowError("Account has insufficient funds");
  }

  // Send
  string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
  if (strError != "") {
    return NanThrowError(strError.c_str());
  }

  std::string tx_hash = wtx.GetHash().GetHex();

  NanReturnValue(NanNew<String>(tx_hash));
}

NAN_METHOD(ListTransactions) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.listTransactions(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(ListAccounts) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.listAccounts(options)");
  }

  Local<Object> options = Local<Object>::Cast(args[0]);

  int nMinDepth = 1;
  if (options->Get(NanNew<String>("minDepth"))->IsNumber()) {
    nMinDepth = options->Get(NanNew<String>("minDepth"))->IntegerValue();
  }

  map<string, int64_t> mapAccountBalances;
  BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& entry, pwalletMain->mapAddressBook) {
    if (IsMine(*pwalletMain, entry.first)) { // This address belongs to me
      mapAccountBalances[entry.second.name] = 0;
    }
  }

  for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
      it != pwalletMain->mapWallet.end(); ++it) {
    const CWalletTx& wtx = (*it).second;
    int64_t nFee;
    string strSentAccount;
    list<pair<CTxDestination, int64_t> > listReceived;
    list<pair<CTxDestination, int64_t> > listSent;
    int nDepth = wtx.GetDepthInMainChain();
    if (wtx.GetBlocksToMaturity() > 0 || nDepth < 0) {
      continue;
    }
    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount);
    mapAccountBalances[strSentAccount] -= nFee;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64_t)& s, listSent) {
      mapAccountBalances[strSentAccount] -= s.second;
    }
    if (nDepth >= nMinDepth) {
      BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64_t)& r, listReceived) {
        if (pwalletMain->mapAddressBook.count(r.first)) {
          mapAccountBalances[pwalletMain->mapAddressBook[r.first].name] += r.second;
        } else {
          mapAccountBalances[""] += r.second;
        }
      }
    }
  }

  list<CAccountingEntry> acentries;
  CWalletDB(pwalletMain->strWalletFile).ListAccountCreditDebit("*", acentries);
  BOOST_FOREACH(const CAccountingEntry& entry, acentries) {
    mapAccountBalances[entry.strAccount] += entry.nCreditDebit;
  }

  Local<Object> obj = NanNew<Object>();
  BOOST_FOREACH(const PAIRTYPE(string, int64_t)& accountBalance, mapAccountBalances) {
    Local<Object> entry = NanNew<Object>();
    entry->Set(NanNew<String>("balance"), NanNew<Number>(accountBalance.second));
    Local<Array> addr = NanNew<Array>();
    int i = 0;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, CAddressBookData)& item, pwalletMain->mapAddressBook) {
      const CBitcoinAddress& address = item.first;
      const string& strName = item.second.name;
      if (strName == accountBalance.first) {
        Local<Object> a = NanNew<Object>();
        a->Set(NanNew<String>("address"), NanNew<String>(address.ToString()));

        CKeyID keyID;
        if (!address.GetKeyID(keyID)) {
          return NanThrowError("Address does not refer to a key");
        }
        CKey vchSecret;
        if (!pwalletMain->GetKey(keyID, vchSecret)) {
          return NanThrowError("Private key for address is not known");
        }
        std::string priv = CBitcoinSecret(vchSecret).ToString();
        a->Set(NanNew<String>("privkeycompressed"), NanNew<Boolean>(vchSecret.IsCompressed()));
        a->Set(NanNew<String>("privkey"), NanNew<String>(priv));

        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        a->Set(NanNew<String>("pubkeycompressed"), NanNew<Boolean>(vchPubKey.IsCompressed()));
        a->Set(NanNew<String>("pubkey"), NanNew<String>(HexStr(vchPubKey)));

        addr->Set(i, a);
        i++;
      }
    }
    entry->Set(NanNew<String>("addresses"), addr);
    obj->Set(NanNew<String>(accountBalance.first), entry);
  }

  NanReturnValue(obj);
}

NAN_METHOD(GetTransaction) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.getTransaction(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(BackupWallet) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.backupWallet(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(WalletPassphrase) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.walletPassphrase(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(WalletPassphraseChange) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.walletPassphraseChange(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(WalletLock) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.walletLock(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(EncryptWallet) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.encryptWallet(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

NAN_METHOD(SetTxFee) {
  NanScope();

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return NanThrowError(
      "Usage: bitcoindjs.setTxFee(options)");
  }

  // Parse the account first so we don't generate a key if there's an error
  Local<Object> options = Local<Object>::Cast(args[0]);
  String::Utf8Value name_(options->Get(NanNew<String>("name"))->ToString());
  std::string strAccount = std::string(*name_);

  NanReturnValue(Undefined());
}

/**
 * Conversions
 */

static inline void
cblock_to_js(const CBlock& block, const CBlockIndex* blockindex, Local<Object> obj) {
  obj->Set(NanNew<String>("hash"), NanNew<String>(block.GetHash().GetHex().c_str()));
  CMerkleTx txGen(block.vtx[0]);
  txGen.SetMerkleBranch(&block);
  obj->Set(NanNew<String>("confirmations"), NanNew<Number>((int)txGen.GetDepthInMainChain()));
  obj->Set(NanNew<String>("size"), NanNew<Number>((int)::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
  obj->Set(NanNew<String>("height"), NanNew<Number>(blockindex->nHeight));
  obj->Set(NanNew<String>("version"), NanNew<Number>(block.nVersion));
  obj->Set(NanNew<String>("merkleroot"), NanNew<String>(block.hashMerkleRoot.GetHex()));

  Local<Array> txs = NanNew<Array>();
  int ti = 0;
  BOOST_FOREACH(const CTransaction& tx, block.vtx) {
    Local<Object> entry = NanNew<Object>();

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;
    string strHex = HexStr(ssTx.begin(), ssTx.end());
    entry->Set(NanNew<String>("hex"), NanNew<String>(strHex));

    entry->Set(NanNew<String>("txid"), NanNew<String>(tx.GetHash().GetHex()));
    entry->Set(NanNew<String>("version"), NanNew<Number>(tx.nVersion));
    entry->Set(NanNew<String>("locktime"), NanNew<Number>(tx.nLockTime));

    Local<Array> vin = NanNew<Array>();
    int vi = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin) {
      Local<Object> in = NanNew<Object>();
      if (tx.IsCoinBase()) {
        in->Set(NanNew<String>("coinbase"), NanNew<String>(HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
      } else {
        in->Set(NanNew<String>("txid"), NanNew<String>(txin.prevout.hash.GetHex()));
        in->Set(NanNew<String>("vout"), NanNew<Number>((boost::int64_t)txin.prevout.n));
        Local<Object> o = NanNew<Object>();
        o->Set(NanNew<String>("asm"), NanNew<String>(txin.scriptSig.ToString()));
        o->Set(NanNew<String>("hex"), NanNew<String>(HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        in->Set(NanNew<String>("scriptSig"), o);
      }
      in->Set(NanNew<String>("sequence"), NanNew<Number>((boost::int64_t)txin.nSequence));
      vin->Set(vi, in);
      vi++;
    }
    entry->Set(NanNew<String>("vin"), vin);

    Local<Array> vout = NanNew<Array>();
    for (unsigned int vo = 0; vo < tx.vout.size(); vo++) {
      const CTxOut& txout = tx.vout[vo];
      Local<Object> out = NanNew<Object>();
      out->Set(NanNew<String>("value"), NanNew<Number>(txout.nValue));
      out->Set(NanNew<String>("n"), NanNew<Number>((boost::int64_t)vo));

      Local<Object> o = NanNew<Object>();
      {
        const CScript& scriptPubKey = txout.scriptPubKey;
        Local<Object> out = o;
        bool fIncludeHex = true;

        txnouttype type;
        vector<CTxDestination> addresses;
        int nRequired;
        out->Set(NanNew<String>("asm"), NanNew<String>(scriptPubKey.ToString()));
        if (fIncludeHex) {
          out->Set(NanNew<String>("hex"), NanNew<String>(HexStr(scriptPubKey.begin(), scriptPubKey.end())));
        }
        if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
          out->Set(NanNew<String>("type"), NanNew<String>(GetTxnOutputType(type)));
        } else {
          out->Set(NanNew<String>("reqSigs"), NanNew<Number>(nRequired));
          out->Set(NanNew<String>("type"), NanNew<String>(GetTxnOutputType(type)));
          Local<Array> a = NanNew<Array>();
          int ai = 0;
          BOOST_FOREACH(const CTxDestination& addr, addresses) {
            a->Set(ai, NanNew<String>(CBitcoinAddress(addr).ToString()));
            ai++;
          }
          out->Set(NanNew<String>("addresses"), a);
        }
      }
      out->Set(NanNew<String>("scriptPubKey"), o);

      vout->Set(vo, out);
    }
    entry->Set(NanNew<String>("vout"), vout);

    {
      const uint256 hashBlock = block.GetHash();
      if (hashBlock != 0) {
        entry->Set(NanNew<String>("blockhash"), NanNew<String>(hashBlock.GetHex()));
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
          CBlockIndex* pindex = (*mi).second;
          if (chainActive.Contains(pindex)) {
            entry->Set(NanNew<String>("confirmations"),
              NanNew<Number>(1 + chainActive.Height() - pindex->nHeight));
            entry->Set(NanNew<String>("time"), NanNew<Number>((boost::int64_t)pindex->nTime));
            entry->Set(NanNew<String>("blocktime"), NanNew<Number>((boost::int64_t)pindex->nTime));
          } else {
            entry->Set(NanNew<String>("confirmations"), NanNew<Number>(0));
          }
        }
      }
    }

    txs->Set(ti, entry);
    ti++;
  }
  obj->Set(NanNew<String>("tx"), txs);

  obj->Set(NanNew<String>("time"), NanNew<Number>((boost::int64_t)block.GetBlockTime()));
  obj->Set(NanNew<String>("nonce"), NanNew<Number>((boost::uint64_t)block.nNonce));
  obj->Set(NanNew<String>("bits"), NanNew<Number>(block.nBits));
  obj->Set(NanNew<String>("difficulty"), NanNew<Number>(GetDifficulty(blockindex)));
  obj->Set(NanNew<String>("chainwork"), NanNew<String>(blockindex->nChainWork.GetHex()));
  if (blockindex->pprev) {
    obj->Set(NanNew<String>("previousblockhash"), NanNew<String>(blockindex->pprev->GetBlockHash().GetHex()));
  }
  CBlockIndex *pnext = chainActive.Next(blockindex);
  if (pnext) {
    obj->Set(NanNew<String>("nextblockhash"), NanNew<String>(pnext->GetBlockHash().GetHex()));
  }
}

static inline void
ctx_to_js(const CTransaction& tx, uint256 hashBlock, Local<Object> entry) {
  CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
  ssTx << tx;
  string strHex = HexStr(ssTx.begin(), ssTx.end());
  entry->Set(NanNew<String>("hex"), NanNew<String>(strHex));

  entry->Set(NanNew<String>("txid"), NanNew<String>(tx.GetHash().GetHex()));
  entry->Set(NanNew<String>("version"), NanNew<Number>(tx.nVersion));
  entry->Set(NanNew<String>("locktime"), NanNew<Number>(tx.nLockTime));

  Local<Array> vin = NanNew<Array>();
  int vi = 0;
  BOOST_FOREACH(const CTxIn& txin, tx.vin) {
    Local<Object> in = NanNew<Object>();
    if (tx.IsCoinBase()) {
      in->Set(NanNew<String>("coinbase"), NanNew<String>(HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
    } else {
      in->Set(NanNew<String>("txid"), NanNew<String>(txin.prevout.hash.GetHex()));
      in->Set(NanNew<String>("vout"), NanNew<Number>((boost::int64_t)txin.prevout.n));
      Local<Object> o = NanNew<Object>();
      o->Set(NanNew<String>("asm"), NanNew<String>(txin.scriptSig.ToString()));
      o->Set(NanNew<String>("hex"), NanNew<String>(HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
      in->Set(NanNew<String>("scriptSig"), o);
    }
    in->Set(NanNew<String>("sequence"), NanNew<Number>((boost::int64_t)txin.nSequence));
    vin->Set(vi, in);
    vi++;
  }
  entry->Set(NanNew<String>("vin"), vin);

  Local<Array> vout = NanNew<Array>();
  for (unsigned int vo = 0; vo < tx.vout.size(); vo++) {
    const CTxOut& txout = tx.vout[vo];
    Local<Object> out = NanNew<Object>();
    out->Set(NanNew<String>("value"), NanNew<Number>(txout.nValue));
    out->Set(NanNew<String>("n"), NanNew<Number>((boost::int64_t)vo));

    Local<Object> o = NanNew<Object>();
    {
      const CScript& scriptPubKey = txout.scriptPubKey;
      Local<Object> out = o;
      bool fIncludeHex = true;

      txnouttype type;
      vector<CTxDestination> addresses;
      int nRequired;
      out->Set(NanNew<String>("asm"), NanNew<String>(scriptPubKey.ToString()));
      if (fIncludeHex) {
        out->Set(NanNew<String>("hex"), NanNew<String>(HexStr(scriptPubKey.begin(), scriptPubKey.end())));
      }
      if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        out->Set(NanNew<String>("type"), NanNew<String>(GetTxnOutputType(type)));
      } else {
        out->Set(NanNew<String>("reqSigs"), NanNew<Number>(nRequired));
        out->Set(NanNew<String>("type"), NanNew<String>(GetTxnOutputType(type)));
        Local<Array> a = NanNew<Array>();
        int ai = 0;
        BOOST_FOREACH(const CTxDestination& addr, addresses) {
          a->Set(ai, NanNew<String>(CBitcoinAddress(addr).ToString()));
          ai++;
        }
        out->Set(NanNew<String>("addresses"), a);
      }
    }
    out->Set(NanNew<String>("scriptPubKey"), o);

    vout->Set(vo, out);
  }
  entry->Set(NanNew<String>("vout"), vout);

  if (hashBlock != 0) {
    entry->Set(NanNew<String>("blockhash"), NanNew<String>(hashBlock.GetHex()));
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end() && (*mi).second) {
      CBlockIndex* pindex = (*mi).second;
      if (chainActive.Contains(pindex)) {
        entry->Set(NanNew<String>("confirmations"),
          NanNew<Number>(1 + chainActive.Height() - pindex->nHeight));
        entry->Set(NanNew<String>("time"), NanNew<Number>((boost::int64_t)pindex->nTime));
        entry->Set(NanNew<String>("blocktime"), NanNew<Number>((boost::int64_t)pindex->nTime));
      } else {
        entry->Set(NanNew<String>("confirmations"), NanNew<Number>(0));
      }
    }
  }
}

/**
 * Init
 */

extern "C" void
init(Handle<Object> target) {
  NanScope();
  NODE_SET_METHOD(target, "start", StartBitcoind);
  NODE_SET_METHOD(target, "stop", StopBitcoind);
  NODE_SET_METHOD(target, "stopping", IsStopping);
  NODE_SET_METHOD(target, "stopped", IsStopped);
  NODE_SET_METHOD(target, "getBlock", GetBlock);
  NODE_SET_METHOD(target, "getTx", GetTx);
  NODE_SET_METHOD(target, "pollBlocks", PollBlocks);
  NODE_SET_METHOD(target, "pollMempool", PollMempool);
  NODE_SET_METHOD(target, "broadcastTx", BroadcastTx);
  NODE_SET_METHOD(target, "verifyBlock", VerifyBlock);
  NODE_SET_METHOD(target, "verifyTransaction", VerifyTransaction);

  NODE_SET_METHOD(target, "walletNewAddress", WalletNewAddress);
  NODE_SET_METHOD(target, "getAccountAddress", GetAccountAddress);
  NODE_SET_METHOD(target, "setAccount", SetAccount);
  NODE_SET_METHOD(target, "getAccount", GetAccount);
  NODE_SET_METHOD(target, "sendToAddress", SendToAddress);
  NODE_SET_METHOD(target, "signMessage", SignMessage);
  NODE_SET_METHOD(target, "verifyMessage", VerifyMessage);
  NODE_SET_METHOD(target, "getBalance", GetBalance);
  NODE_SET_METHOD(target, "getUnconfirmedBalance", GetUnconfirmedBalance);
  NODE_SET_METHOD(target, "sendFrom", SendFrom);
  NODE_SET_METHOD(target, "listTransactions", ListTransactions);
  NODE_SET_METHOD(target, "listAccounts", ListAccounts);
  NODE_SET_METHOD(target, "getTransaction", GetTransaction);
  NODE_SET_METHOD(target, "backupWallet", BackupWallet);
  NODE_SET_METHOD(target, "walletPassphrase", WalletPassphrase);
  NODE_SET_METHOD(target, "walletPassphraseChange", WalletPassphraseChange);
  NODE_SET_METHOD(target, "walletLock", WalletLock);
  NODE_SET_METHOD(target, "encryptWallet", EncryptWallet);
  NODE_SET_METHOD(target, "setTxFee", SetTxFee);
}

NODE_MODULE(bitcoindjs, init)