/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "android-base/stringprintf.h"

#include "base/atomic.h"
#include "base/logging.h"  // For VLOG.
#include "base/time_utils.h"
#include "debugger.h"
#include "jdwp/jdwp_priv.h"
#include "scoped_thread_state_change-inl.h"

namespace art {

namespace JDWP {

using android::base::StringPrintf;

static void* StartJdwpThread(void* arg);


static bool ParseJdwpOption(const std::string& name,
                            const std::string& value,
                            JdwpOptions* jdwp_options) {
  if (name == "transport") {
    if (value == "dt_socket") {
      jdwp_options->transport = JDWP::kJdwpTransportSocket;
    } else if (value == "dt_android_adb") {
      jdwp_options->transport = JDWP::kJdwpTransportAndroidAdb;
    } else {
      jdwp_options->transport = JDWP::kJdwpTransportUnknown;
      LOG(ERROR) << "JDWP transport not supported: " << value;
      return false;
    }
  } else if (name == "server") {
    if (value == "n") {
      jdwp_options->server = false;
    } else if (value == "y") {
      jdwp_options->server = true;
    } else {
      LOG(ERROR) << "JDWP option 'server' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "suspend") {
    if (value == "n") {
      jdwp_options->suspend = false;
    } else if (value == "y") {
      jdwp_options->suspend = true;
    } else {
      LOG(ERROR) << "JDWP option 'suspend' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "address") {
    /* this is either <port> or <host>:<port> */
    std::string port_string;
    jdwp_options->host.clear();
    std::string::size_type colon = value.find(':');
    if (colon != std::string::npos) {
      jdwp_options->host = value.substr(0, colon);
      port_string = value.substr(colon + 1);
    } else {
      port_string = value;
    }
    if (port_string.empty()) {
      LOG(ERROR) << "JDWP address missing port: " << value;
      return false;
    }
    char* end;
    uint64_t port = strtoul(port_string.c_str(), &end, 10);
    if (*end != '\0' || port > 0xffff) {
      LOG(ERROR) << "JDWP address has junk in port field: " << value;
      return false;
    }
    jdwp_options->port = port;
  } else if (name == "launch" || name == "onthrow" || name == "oncaught" || name == "timeout") {
    /* valid but unsupported */
    LOG(INFO) << "Ignoring JDWP option '" << name << "'='" << value << "'";
  } else {
    LOG(INFO) << "Ignoring unrecognized JDWP option '" << name << "'='" << value << "'";
  }

  return true;
}

bool ParseJdwpOptions(const std::string& options, JdwpOptions* jdwp_options) {
  VLOG(jdwp) << "ParseJdwpOptions: " << options;

  if (options == "help") {
    LOG(ERROR) << "Example: -XjdwpOptions:transport=dt_socket,address=8000,server=y\n"
               << "Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
               << "Example: -Xrunjdwp:transport=dt_socket,address=localhost:6500,server=n\n";
    return false;
  }

  const std::string s;

  std::vector<std::string> pairs;
  Split(options, ',', &pairs);

  for (const std::string& jdwp_option : pairs) {
    std::string::size_type equals_pos = jdwp_option.find('=');
    if (equals_pos == std::string::npos) {
      LOG(ERROR) << s << "Can't parse JDWP option '" << jdwp_option << "' in '" << options << "'";
      return false;
    }

    bool parse_attempt = ParseJdwpOption(jdwp_option.substr(0, equals_pos),
                                         jdwp_option.substr(equals_pos + 1),
                                         jdwp_options);
    if (!parse_attempt) {
      // We fail to parse this JDWP option.
      return parse_attempt;
    }
  }

  if (jdwp_options->transport == JDWP::kJdwpTransportUnknown) {
    LOG(ERROR) << s << "Must specify JDWP transport: " << options;
    return false;
  }
#if ART_TARGET_ANDROID
  if (jdwp_options->transport == JDWP::kJdwpTransportNone) {
    jdwp_options->transport = JDWP::kJdwpTransportAndroidAdb;
    LOG(WARNING) << "no JDWP transport specified. Defaulting to dt_android_adb";
  }
#endif
  if (!jdwp_options->server && (jdwp_options->host.empty() || jdwp_options->port == 0)) {
    LOG(ERROR) << s << "Must specify JDWP host and port when server=n: " << options;
    return false;
  }

  return true;
}

/*
 * JdwpNetStateBase class implementation
 */
JdwpNetStateBase::JdwpNetStateBase(JdwpState* state)
    : state_(state), socket_lock_("JdwpNetStateBase lock", kJdwpSocketLock) {
  clientSock = -1;
  wake_pipe_[0] = -1;
  wake_pipe_[1] = -1;
  input_count_ = 0;
  awaiting_handshake_ = false;
}

JdwpNetStateBase::~JdwpNetStateBase() {
  if (wake_pipe_[0] != -1) {
    close(wake_pipe_[0]);
    wake_pipe_[0] = -1;
  }
  if (wake_pipe_[1] != -1) {
    close(wake_pipe_[1]);
    wake_pipe_[1] = -1;
  }
}

bool JdwpNetStateBase::MakePipe() {
  if (pipe(wake_pipe_) == -1) {
    PLOG(ERROR) << "pipe failed";
    return false;
  }
  return true;
}

void JdwpNetStateBase::WakePipe() {
  // If we might be sitting in select, kick us loose.
  if (wake_pipe_[1] != -1) {
    VLOG(jdwp) << "+++ writing to wake pipe";
    TEMP_FAILURE_RETRY(write(wake_pipe_[1], "", 1));
  }
}

void JdwpNetStateBase::ConsumeBytes(size_t count) {
  CHECK_GT(count, 0U);
  CHECK_LE(count, input_count_);

  if (count == input_count_) {
    input_count_ = 0;
    return;
  }

  memmove(input_buffer_, input_buffer_ + count, input_count_ - count);
  input_count_ -= count;
}

bool JdwpNetStateBase::HaveFullPacket() {
  if (awaiting_handshake_) {
    return (input_count_ >= kMagicHandshakeLen);
  }
  if (input_count_ < 4) {
    return false;
  }
  uint32_t length = Get4BE(input_buffer_);
  return (input_count_ >= length);
}

bool JdwpNetStateBase::IsAwaitingHandshake() {
  return awaiting_handshake_;
}

void JdwpNetStateBase::SetAwaitingHandshake(bool new_state) {
  awaiting_handshake_ = new_state;
}

bool JdwpNetStateBase::IsConnected() {
  return clientSock >= 0;
}

// Close a connection from a debugger (which may have already dropped us).
// Resets the state so we're ready to receive a new connection.
// Only called from the JDWP thread.
void JdwpNetStateBase::Close() {
  if (clientSock < 0) {
    return;
  }

  VLOG(jdwp) << "+++ closing JDWP connection on fd " << clientSock;

  close(clientSock);
  clientSock = -1;
}

/*
 * Write a packet of "length" bytes. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::WritePacket(ExpandBuf* pReply, size_t length) {
  DCHECK_LE(length, expandBufGetLength(pReply));
  if (!IsConnected()) {
    LOG(WARNING) << "Connection with debugger is closed";
    return -1;
  }
  MutexLock mu(Thread::Current(), socket_lock_);
  return TEMP_FAILURE_RETRY(write(clientSock, expandBufGetBuffer(pReply), length));
}

/*
 * Write a buffered packet. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::WriteBufferedPacket(const std::vector<iovec>& iov) {
  MutexLock mu(Thread::Current(), socket_lock_);
  return WriteBufferedPacketLocked(iov);
}

ssize_t JdwpNetStateBase::WriteBufferedPacketLocked(const std::vector<iovec>& iov) {
  socket_lock_.AssertHeld(Thread::Current());
  DCHECK(IsConnected()) << "Connection with debugger is closed";
  return TEMP_FAILURE_RETRY(writev(clientSock, &iov[0], iov.size()));
}

bool JdwpState::IsConnected() {
  return netState != nullptr && netState->IsConnected();
}

void JdwpState::SendBufferedRequest(uint32_t type, const std::vector<iovec>& iov) {
  if (!IsConnected()) {
    // Can happen with some DDMS events.
    VLOG(jdwp) << "Not sending JDWP packet: no debugger attached!";
    return;
  }

  size_t expected = 0;
  for (size_t i = 0; i < iov.size(); ++i) {
    expected += iov[i].iov_len;
  }

  errno = 0;
  ssize_t actual = netState->WriteBufferedPacket(iov);
  if (static_cast<size_t>(actual) != expected) {
    PLOG(ERROR) << StringPrintf("Failed to send JDWP packet %c%c%c%c to debugger (%zd of %zu)",
                                static_cast<char>(type >> 24),
                                static_cast<char>(type >> 16),
                                static_cast<char>(type >> 8),
                                static_cast<char>(type),
                                actual, expected);
  }
}

void JdwpState::SendRequest(ExpandBuf* pReq) {
  if (!IsConnected()) {
    // Can happen with some DDMS events.
    VLOG(jdwp) << "Not sending JDWP packet: no debugger attached!";
    return;
  }

  errno = 0;
  ssize_t actual = netState->WritePacket(pReq, expandBufGetLength(pReq));
  if (static_cast<size_t>(actual) != expandBufGetLength(pReq)) {
    PLOG(ERROR) << StringPrintf("Failed to send JDWP packet to debugger (%zd of %zu)",
                                actual, expandBufGetLength(pReq));
  }
}

/*
 * Get the next "request" serial number.  We use this when sending
 * packets to the debugger.
 */
uint32_t JdwpState::NextRequestSerial() {
  return request_serial_++;
}

/*
 * Get the next "event" serial number.  We use this in the response to
 * message type EventRequest.Set.
 */
uint32_t JdwpState::NextEventSerial() {
  return event_serial_++;
}

JdwpState::JdwpState(const JdwpOptions* options)
    : options_(options),
      thread_start_lock_("JDWP thread start lock", kJdwpStartLock),
      thread_start_cond_("JDWP thread start condition variable", thread_start_lock_),
      pthread_(0),
      thread_(nullptr),
      debug_thread_started_(false),
      debug_thread_id_(0),
      run(false),
      netState(nullptr),
      attach_lock_("JDWP attach lock", kJdwpAttachLock),
      attach_cond_("JDWP attach condition variable", attach_lock_),
      last_activity_time_ms_(0),
      request_serial_(0x10000000),
      event_serial_(0x20000000),
      event_list_lock_("JDWP event list lock", kJdwpEventListLock),
      event_list_(nullptr),
      event_list_size_(0),
      jdwp_token_lock_("JDWP token lock"),
      jdwp_token_cond_("JDWP token condition variable", jdwp_token_lock_),
      jdwp_token_owner_thread_id_(0),
      ddm_is_active_(false),
      should_exit_(false),
      exit_status_(0),
      shutdown_lock_("JDWP shutdown lock", kJdwpShutdownLock),
      shutdown_cond_("JDWP shutdown condition variable", shutdown_lock_),
      processing_request_(false) {
  Locks::AddToExpectedMutexesOnWeakRefAccess(&event_list_lock_);
}

/*
 * Initialize JDWP.
 *
 * Does not return until JDWP thread is running, but may return before
 * the thread is accepting network connections.
 */
JdwpState* JdwpState::Create(const JdwpOptions* options) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  std::unique_ptr<JdwpState> state(new JdwpState(options));
  switch (options->transport) {
    case kJdwpTransportSocket:
      InitSocketTransport(state.get(), options);
      break;
#ifdef ART_TARGET_ANDROID
    case kJdwpTransportAndroidAdb:
      InitAdbTransport(state.get(), options);
      break;
#endif
    default:
      LOG(FATAL) << "Unknown transport: " << options->transport;
  }
  {
    /*
     * Grab a mutex before starting the thread.  This ensures they
     * won't signal the cond var before we're waiting.
     */
    state->thread_start_lock_.AssertNotHeld(self);
    MutexLock thread_start_locker(self, state->thread_start_lock_);

    /*
     * We have bound to a port, or are trying to connect outbound to a
     * debugger.  Create the JDWP thread and let it continue the mission.
     */
    CHECK_PTHREAD_CALL(pthread_create, (&state->pthread_, nullptr, StartJdwpThread, state.get()),
                       "JDWP thread");

    /*
     * Wait until the thread finishes basic initialization.
     */
    while (!state->debug_thread_started_) {
      state->thread_start_cond_.Wait(self);
    }
  }

  if (options->suspend) {
    /*
     * For suspend=y, wait for the debugger to connect to us or for us to
     * connect to the debugger.
     *
     * The JDWP thread will signal us when it connects successfully or
     * times out (for timeout=xxx), so we have to check to see what happened
     * when we wake up.
     */
    {
      ScopedThreadStateChange tsc(self, kWaitingForDebuggerToAttach);
      MutexLock attach_locker(self, state->attach_lock_);
      while (state->debug_thread_id_ == 0) {
        state->attach_cond_.Wait(self);
      }
    }
    if (!state->IsActive()) {
      LOG(ERROR) << "JDWP connection failed";
      return nullptr;
    }

    LOG(INFO) << "JDWP connected";

    /*
     * Ordinarily we would pause briefly to allow the debugger to set
     * breakpoints and so on, but for "suspend=y" the VM init code will
     * pause the VM when it sends the VM_START message.
     */
  }

  return state.release();
}

/*
 * Reset all session-related state.  There should not be an active connection
 * to the client at this point.  The rest of the VM still thinks there is
 * a debugger attached.
 *
 * This includes freeing up the debugger event list.
 */
void JdwpState::ResetState() {
  /* could reset the serial numbers, but no need to */

  UnregisterAll();
  {
    MutexLock mu(Thread::Current(), event_list_lock_);
    CHECK(event_list_ == nullptr);
  }

  /*
   * Should not have one of these in progress.  If the debugger went away
   * mid-request, though, we could see this.
   */
  if (jdwp_token_owner_thread_id_ != 0) {
    LOG(WARNING) << "Resetting state while event in progress";
    DCHECK(false);
  }
}

/*
 * Tell the JDWP thread to shut down.  Frees "state".
 */
JdwpState::~JdwpState() {
  if (netState != nullptr) {
    /*
     * Close down the network to inspire the thread to halt. If a request is being processed,
     * we need to wait for it to finish first.
     */
    {
      Thread* self = Thread::Current();
      MutexLock mu(self, shutdown_lock_);
      while (processing_request_) {
        VLOG(jdwp) << "JDWP command in progress: wait for it to finish ...";
        shutdown_cond_.Wait(self);
      }

      VLOG(jdwp) << "JDWP shutting down net...";
      netState->Shutdown();
    }

    if (debug_thread_started_) {
      run = false;
      void* threadReturn;
      if (pthread_join(pthread_, &threadReturn) != 0) {
        LOG(WARNING) << "JDWP thread join failed";
      }
    }

    VLOG(jdwp) << "JDWP freeing netstate...";
    delete netState;
    netState = nullptr;
  }
  CHECK(netState == nullptr);

  ResetState();

  Locks::RemoveFromExpectedMutexesOnWeakRefAccess(&event_list_lock_);
}

/*
 * Are we talking to a debugger?
 */
bool JdwpState::IsActive() {
  return IsConnected();
}

// Returns "false" if we encounter a connection-fatal error.
bool JdwpState::HandlePacket() {
  Thread* const self = Thread::Current();
  {
    MutexLock mu(self, shutdown_lock_);
    processing_request_ = true;
  }
  JdwpNetStateBase* netStateBase = netState;
  CHECK(netStateBase != nullptr) << "Connection has been closed";
  JDWP::Request request(netStateBase->input_buffer_, netStateBase->input_count_);

  ExpandBuf* pReply = expandBufAlloc();
  bool skip_reply = false;
  size_t replyLength = ProcessRequest(&request, pReply, &skip_reply);
  ssize_t cc = 0;
  if (!skip_reply) {
    cc = netStateBase->WritePacket(pReply, replyLength);
  } else {
    DCHECK_EQ(replyLength, 0U);
  }
  expandBufFree(pReply);

  /*
   * We processed this request and sent its reply so we can release the JDWP token.
   */
  ReleaseJdwpTokenForCommand();

  if (cc != static_cast<ssize_t>(replyLength)) {
    PLOG(ERROR) << "Failed sending reply to debugger";
    return false;
  }
  netStateBase->ConsumeBytes(request.GetLength());
  {
    MutexLock mu(self, shutdown_lock_);
    processing_request_ = false;
    shutdown_cond_.Broadcast(self);
  }
  return true;
}

/*
 * Entry point for JDWP thread.  The thread was created through the VM
 * mechanisms, so there is a java/lang/Thread associated with us.
 */
static void* StartJdwpThread(void* arg) {
  JdwpState* state = reinterpret_cast<JdwpState*>(arg);
  CHECK(state != nullptr);

  state->Run();
  return nullptr;
}

void JdwpState::Run() {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->AttachCurrentThread("JDWP", true, runtime->GetSystemThreadGroup(),
                                     !runtime->IsAotCompiler()));

  VLOG(jdwp) << "JDWP: thread running";

  /*
   * Finish initializing, then notify the creating thread that
   * we're running.
   */
  thread_ = Thread::Current();
  run = true;

  {
    MutexLock locker(thread_, thread_start_lock_);
    debug_thread_started_ = true;
    thread_start_cond_.Broadcast(thread_);
  }

  /* set the thread state to kWaitingInMainDebuggerLoop so GCs don't wait for us */
  CHECK_EQ(thread_->GetState(), kNative);
  Locks::mutator_lock_->AssertNotHeld(thread_);
  thread_->SetState(kWaitingInMainDebuggerLoop);

  /*
   * Loop forever if we're in server mode, processing connections.  In
   * non-server mode, we bail out of the thread when the debugger drops
   * us.
   *
   * We broadcast a notification when a debugger attaches, after we
   * successfully process the handshake.
   */
  while (run) {
    if (options_->server) {
      /*
       * Block forever, waiting for a connection.  To support the
       * "timeout=xxx" option we'll need to tweak this.
       */
      if (!netState->Accept()) {
        break;
      }
    } else {
      /*
       * If we're not acting as a server, we need to connect out to the
       * debugger.  To support the "timeout=xxx" option we need to
       * have a timeout if the handshake reply isn't received in a
       * reasonable amount of time.
       */
      if (!netState->Establish(options_)) {
        /* wake anybody who was waiting for us to succeed */
        MutexLock mu(thread_, attach_lock_);
        debug_thread_id_ = static_cast<ObjectId>(-1);
        attach_cond_.Broadcast(thread_);
        break;
      }
    }

    /* prep debug code to handle the new connection */
    Dbg::Connected();

    /* process requests until the debugger drops */
    bool first = true;
    while (!Dbg::IsDisposed()) {
      // sanity check -- shouldn't happen?
      CHECK_EQ(thread_->GetState(), kWaitingInMainDebuggerLoop);

      if (!netState->ProcessIncoming()) {
        /* blocking read */
        break;
      }

      if (should_exit_) {
        exit(exit_status_);
      }

      if (first && !netState->IsAwaitingHandshake()) {
        /* handshake worked, tell the interpreter that we're active */
        first = false;

        /* set thread ID; requires object registry to be active */
        {
          ScopedObjectAccess soa(thread_);
          debug_thread_id_ = Dbg::GetThreadSelfId();
        }

        /* wake anybody who's waiting for us */
        MutexLock mu(thread_, attach_lock_);
        attach_cond_.Broadcast(thread_);
      }
    }

    netState->Close();

    if (ddm_is_active_) {
      ddm_is_active_ = false;

      /* broadcast the disconnect; must be in RUNNING state */
      ScopedObjectAccess soa(thread_);
      Dbg::DdmDisconnected();
    }

    {
      ScopedObjectAccess soa(thread_);

      // Release session state, e.g. remove breakpoint instructions.
      ResetState();
    }
    // Tell the rest of the runtime that the debugger is no longer around.
    Dbg::Disconnected();

    /* if we had threads suspended, resume them now */
    Dbg::UndoDebuggerSuspensions();

    /* if we connected out, this was a one-shot deal */
    if (!options_->server) {
      run = false;
    }
  }

  /* back to native, for thread shutdown */
  CHECK_EQ(thread_->GetState(), kWaitingInMainDebuggerLoop);
  thread_->SetState(kNative);

  VLOG(jdwp) << "JDWP: thread detaching and exiting...";
  runtime->DetachCurrentThread();
}

void JdwpState::NotifyDdmsActive() {
  if (!ddm_is_active_) {
    ddm_is_active_ = true;
    Dbg::DdmConnected();
  }
}

Thread* JdwpState::GetDebugThread() {
  return thread_;
}

/*
 * Support routines for waitForDebugger().
 *
 * We can't have a trivial "waitForDebugger" function that returns the
 * instant the debugger connects, because we run the risk of executing code
 * before the debugger has had a chance to configure breakpoints or issue
 * suspend calls.  It would be nice to just sit in the suspended state, but
 * most debuggers don't expect any threads to be suspended when they attach.
 *
 * There's no JDWP event we can post to tell the debugger, "we've stopped,
 * and we like it that way".  We could send a fake breakpoint, which should
 * cause the debugger to immediately send a resume, but the debugger might
 * send the resume immediately or might throw an exception of its own upon
 * receiving a breakpoint event that it didn't ask for.
 *
 * What we really want is a "wait until the debugger is done configuring
 * stuff" event.  We can approximate this with a "wait until the debugger
 * has been idle for a brief period".
 */

/*
 * Return the time, in milliseconds, since the last debugger activity.
 *
 * Returns -1 if no debugger is attached, or 0 if we're in the middle of
 * processing a debugger request.
 */
int64_t JdwpState::LastDebuggerActivity() {
  if (!Dbg::IsDebuggerActive()) {
    LOG(WARNING) << "no active debugger";
    return -1;
  }

  int64_t last = last_activity_time_ms_.LoadSequentiallyConsistent();

  /* initializing or in the middle of something? */
  if (last == 0) {
    VLOG(jdwp) << "+++ last=busy";
    return 0;
  }

  /* now get the current time */
  int64_t now = MilliTime();
  CHECK_GE(now, last);

  VLOG(jdwp) << "+++ debugger interval=" << (now - last);
  return now - last;
}

void JdwpState::ExitAfterReplying(int exit_status) {
  LOG(WARNING) << "Debugger told VM to exit with status " << exit_status;
  should_exit_ = true;
  exit_status_ = exit_status;
}

std::ostream& operator<<(std::ostream& os, const JdwpLocation& rhs) {
  os << "JdwpLocation["
     << Dbg::GetClassName(rhs.class_id) << "." << Dbg::GetMethodName(rhs.method_id)
     << "@" << StringPrintf("%#" PRIx64, rhs.dex_pc) << " " << rhs.type_tag << "]";
  return os;
}

bool operator==(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return lhs.dex_pc == rhs.dex_pc && lhs.method_id == rhs.method_id &&
      lhs.class_id == rhs.class_id && lhs.type_tag == rhs.type_tag;
}

bool operator!=(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return !(lhs == rhs);
}

bool operator==(const JdwpOptions& lhs, const JdwpOptions& rhs) {
  if (&lhs == &rhs) {
    return true;
  }

  return lhs.transport == rhs.transport &&
      lhs.server == rhs.server &&
      lhs.suspend == rhs.suspend &&
      lhs.host == rhs.host &&
      lhs.port == rhs.port;
}

}  // namespace JDWP

}  // namespace art
