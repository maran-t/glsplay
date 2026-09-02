#include "net/peer_session.h"

#include <chrono>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/rtp_sender_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include "audio/loopback_audio_device.h"
#include "capture/desktop_capture_source.h"
#include "encode/nvenc_encoder_factory.h"
#include "encode/nvenc_video_encoder.h"
#include "util/json.h"
#include "util/log.h"

namespace glsplay {
namespace {

constexpr char kVideoTrackId[] = "glsplay-video";
constexpr char kAudioTrackId[] = "glsplay-audio";
constexpr char kStreamId[] = "glsplay";

// libwebrtc wants observers as refcounted objects; these adapt our lambdas.
class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  // Named Callback, not OnSuccess: the base class has a virtual OnSuccess()
  // and a typedef of the same name shadows it, which MSVC reports as a
  // redefinition and then fails to parse the rest of the class.
  using Callback = std::function<void(webrtc::SessionDescriptionInterface*)>;

  static webrtc::scoped_refptr<CreateSdpObserver> Create(Callback on_success) {
    return webrtc::make_ref_counted<CreateSdpObserver>(std::move(on_success));
  }

  explicit CreateSdpObserver(Callback on_success) : on_success_(std::move(on_success)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    if (on_success_) on_success_(desc);
  }
  void OnFailure(webrtc::RTCError error) override {
    LOG_ERROR << "createOffer/Answer failed: " << error.message();
  }

 private:
  Callback on_success_;
};

class SetLocalObserver : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<SetLocalObserver> Create(const char* what) {
    return webrtc::make_ref_counted<SetLocalObserver>(what);
  }
  explicit SetLocalObserver(const char* what) : what_(what) {}

  void OnSuccess() override { LOG_DEBUG << what_ << " applied"; }
  void OnFailure(webrtc::RTCError error) override {
    LOG_ERROR << what_ << " failed: " << error.message();
  }

 private:
  const char* what_;
};

}  // namespace

PeerSession::PeerSession(const HostConfig& config,
                         webrtc::scoped_refptr<DesktopCaptureSource> capture,
                         std::shared_ptr<InputDispatcher> input,
                         SendSignal send)
    : config_(config),
      capture_(std::move(capture)),
      input_(std::move(input)),
      send_(std::move(send)) {}

PeerSession::~PeerSession() {
  ClosePeer();
  factory_ = nullptr;
  if (signaling_thread_) signaling_thread_->Stop();
  if (worker_thread_) worker_thread_->Stop();
  if (network_thread_) network_thread_->Stop();
  webrtc::CleanupSSL();
}

bool PeerSession::Initialise() {
  webrtc::InitializeSSL();

  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("glsplay-net", nullptr);
  if (!network_thread_->Start()) return false;

  worker_thread_ = webrtc::Thread::Create();
  worker_thread_->SetName("glsplay-worker", nullptr);
  if (!worker_thread_->Start()) return false;

  signaling_thread_ = webrtc::Thread::Create();
  signaling_thread_->SetName("glsplay-signaling", nullptr);
  if (!signaling_thread_->Start()) return false;

  auto device = capture_->device();
  if (!device) {
    LOG_ERROR << "capture source has no D3D11 device - start capture first";
    return false;
  }

  auto encoder_factory = std::make_unique<NvencEncoderFactory>(
      device, config_.video.playout_delay_max_ms);
  encoder_factory_ = encoder_factory.get();

  // The ADM must be created on the worker thread; libwebrtc asserts otherwise.
  if (config_.audio.enabled) {
    audio_device_ = worker_thread_->BlockingCall([this] {
      auto adm = LoopbackAudioDevice::Create(config_.audio.sample_rate,
                                             config_.audio.channels);
      adm->Init();
      return adm;
    });
    if (!audio_device_->loopback_ready()) {
      // Drop it entirely rather than handing libwebrtc a device with nothing
      // behind it. Keeping it would leave the audio path live but silent, and
      // libwebrtc asserts its way to an abort the first time it tries to pull
      // a buffer that never arrives.
      LOG_WARN << "no audio endpoint - streaming video only";
      auto* worker = worker_thread_.get();
      worker->BlockingCall([this] { audio_device_ = nullptr; });
    }
  } else {
    LOG_INFO << "audio disabled by configuration";
  }

  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
      audio_device_,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      std::move(encoder_factory),
      webrtc::CreateBuiltinVideoDecoderFactory(),
      /*audio_mixer=*/nullptr,
      /*audio_processing=*/nullptr);

  if (!factory_) {
    LOG_ERROR << "CreatePeerConnectionFactory failed";
    return false;
  }

  LOG_INFO << "libwebrtc PeerConnectionFactory ready (NVENC encoder installed)";
  return true;
}

bool PeerSession::CreatePeerConnection() {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // BUNDLE everything onto one transport so a single port carries audio,
  // video and both data channels - which is also what keeps the firewall rule
  // in PRD section 4.5 to a narrow range.
  rtc_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  rtc_config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

  // Confine media to the range the firewall rules actually open (PRD section
  // 4.5). Without this libwebrtc binds an ephemeral port, gathers a candidate
  // on it, and every packet the browser sends there is dropped - ICE then
  // fails with no indication that a firewall was involved.
  rtc_config.port_allocator_config.min_port = config_.min_port;
  rtc_config.port_allocator_config.max_port = config_.max_port;

  if (!config_.stun_url.empty()) {
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.push_back(config_.stun_url);
    rtc_config.servers.push_back(stun);
  }
  if (!config_.turn_url.empty()) {
    webrtc::PeerConnectionInterface::IceServer turn;
    turn.urls.push_back(config_.turn_url);
    turn.username = config_.turn_username;
    turn.password = config_.turn_credential;
    rtc_config.servers.push_back(turn);
  }

  webrtc::PeerConnectionDependencies deps(this);
  auto result = factory_->CreatePeerConnectionOrError(rtc_config, std::move(deps));
  if (!result.ok()) {
    LOG_ERROR << "CreatePeerConnection failed: " << result.error().message();
    return false;
  }
  peer_ = result.MoveValue();

  // --- video ---------------------------------------------------------------
  auto video_track = factory_->CreateVideoTrack(capture_, kVideoTrackId);
  auto add_video = peer_->AddTrack(video_track, {kStreamId});
  if (!add_video.ok()) {
    LOG_ERROR << "AddTrack(video) failed: " << add_video.error().message();
    return false;
  }

  // Offer the playout-delay header extension. libwebrtc leaves it kStopped by
  // default, and without it in the SDP the per-frame limits the encoder stamps
  // are dropped before they reach the wire. This is the only bound on the
  // receiver's jitter buffer that the receiver has to obey; the client-side
  // jitterBufferTarget=0 is advisory and Chrome overrode it (100-170ms held).
  for (const auto& transceiver : peer_->GetTransceivers()) {
    if (transceiver->media_type() != webrtc::MediaType::VIDEO) continue;
    auto extensions = transceiver->GetHeaderExtensionsToNegotiate();
    bool found = false;
    for (auto& extension : extensions) {
      if (extension.uri != webrtc::RtpExtension::kPlayoutDelayUri) continue;
      extension.direction = webrtc::RtpTransceiverDirection::kSendOnly;
      found = true;
      break;
    }
    if (!found) {
      LOG_WARN << "playout-delay extension not offered by this libwebrtc build";
      break;
    }
    const auto set = transceiver->SetHeaderExtensionsToNegotiate(extensions);
    if (!set.ok()) {
      LOG_WARN << "SetHeaderExtensionsToNegotiate(playout-delay) failed: "
               << set.message();
    } else {
      LOG_INFO << "playout-delay extension offered (0-"
               << config_.video.playout_delay_max_ms << "ms)";
    }
    break;
  }

  // Pin the send parameters. Without an explicit max, libwebrtc starts low and
  // ramps, which on a 20 Mbps target takes several seconds of soft picture.
  auto sender = add_video.value();
  auto params = sender->GetParameters();
  if (!params.encodings.empty()) {
    params.encodings[0].max_bitrate_bps = config_.video.max_bitrate_kbps * 1000;
    params.encodings[0].min_bitrate_bps = 1000 * 1000;
    params.encodings[0].max_framerate = config_.video.fps;
    // Games need frame rate over resolution: dropping to 30fps to keep 1080p
    // feels far worse than holding 60fps at a softer image. This sits on
    // RtpParameters rather than per-encoding - it moved out of
    // RtpEncodingParameters in a later libwebrtc.
    params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
    const auto set = sender->SetParameters(params);
    if (!set.ok()) LOG_WARN << "SetParameters failed: " << set.message();
  }

  // --- audio ---------------------------------------------------------------
  // Only added when there is something to capture. Offering a silent audio
  // track would still cost an m-section and a decoder on the browser side.
  if (audio_device_ && audio_device_->loopback_ready()) {
    webrtc::AudioOptions audio_options;
    // Game audio is not speech: every one of these would fight the mix.
    audio_options.echo_cancellation = false;
    audio_options.auto_gain_control = false;
    audio_options.noise_suppression = false;
    audio_options.highpass_filter = false;

    auto audio_source = factory_->CreateAudioSource(audio_options);
    auto audio_track = factory_->CreateAudioTrack(kAudioTrackId, audio_source.get());
    auto add_audio = peer_->AddTrack(audio_track, {kStreamId});
    if (!add_audio.ok()) {
      LOG_WARN << "AddTrack(audio) failed: " << add_audio.error().message();
    } else {
      LOG_INFO << "audio track added (" << audio_device_->device_name() << ')';
    }
  }

  // --- data channels -------------------------------------------------------
  // The host creates both, so the browser only has to receive them.
  webrtc::DataChannelInit input_init;
  input_init.ordered = false;
  input_init.maxRetransmits = 0;
  input_init.protocol = "glsplay-input-v1";
  auto input_result = peer_->CreateDataChannelOrError("input", &input_init);
  if (input_result.ok()) {
    input_channel_ = input_result.MoveValue();
    input_channel_->RegisterObserver(this);
  } else {
    LOG_ERROR << "input channel creation failed: " << input_result.error().message();
    return false;
  }

  webrtc::DataChannelInit control_init;
  control_init.ordered = true;
  control_init.protocol = "glsplay-control-v1";
  auto control_result = peer_->CreateDataChannelOrError("control", &control_init);
  if (control_result.ok()) {
    control_channel_ = control_result.MoveValue();
    control_channel_->RegisterObserver(this);
  } else {
    LOG_ERROR << "control channel creation failed: " << control_result.error().message();
    return false;
  }

  LOG_INFO << "peer connection created";
  return true;
}

void PeerSession::CreateOffer() {
  if (!peer_ && !CreatePeerConnection()) return;

  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.offer_to_receive_video = 0;
  options.offer_to_receive_audio = 0;

  auto observer = CreateSdpObserver::Create([this](webrtc::SessionDescriptionInterface* desc) {
    std::string sdp;
    if (!desc->ToString(&sdp)) {
      LOG_ERROR << "offer serialisation failed";
      return;
    }
    peer_->SetLocalDescription(SetLocalObserver::Create("local offer").get(), desc);
    send_(std::string("{\"type\":\"offer\",\"sdp\":\"") + json::Escape(sdp) + "\"}");
    LOG_INFO << "offer sent (" << sdp.size() << " bytes)";
  });

  peer_->CreateOffer(observer.get(), options);
}

void PeerSession::HandleAnswer(const std::string& sdp) {
  if (!peer_) {
    LOG_WARN << "answer arrived with no peer connection";
    return;
  }
  webrtc::SdpParseError error;
  auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);
  if (!desc) {
    LOG_ERROR << "answer parse failed at '" << error.line << "': " << error.description;
    return;
  }
  // Whether the client kept the playout-delay extension decides whether the
  // per-frame limits the encoder stamps reach the wire at all. If it is absent
  // they are silently discarded and the receiver goes back to sizing its own
  // jitter buffer, so say so rather than leaving it to be discovered in a dump.
  if (sdp.find(webrtc::RtpExtension::kPlayoutDelayUri) == std::string::npos) {
    LOG_WARN << "client did not accept the playout-delay extension - receiver "
                "jitter buffer is unbounded again";
  } else {
    LOG_INFO << "playout-delay extension negotiated";
  }

  peer_->SetRemoteDescription(SetLocalObserver::Create("remote answer").get(), desc.release());
  LOG_INFO << "answer applied";
}

void PeerSession::HandleRemoteCandidate(const std::string& message) {
  if (!peer_) return;

  const auto candidate = json::GetString(message, "candidate");
  if (!candidate || candidate->empty()) return;
  const auto mid = json::GetString(message, "sdpMid");
  const auto index = json::GetInt(message, "sdpMLineIndex");

  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> ice(webrtc::CreateIceCandidate(
      mid.value_or(""), index.value_or(0), *candidate, &error));
  if (!ice) {
    LOG_WARN << "ICE candidate parse failed: " << error.description;
    return;
  }
  if (!peer_->AddIceCandidate(ice.get())) {
    LOG_WARN << "AddIceCandidate rejected: " << *candidate;
  }
}

void PeerSession::HandleSignalingMessage(const std::string& message) {
  const auto type = json::GetString(message, "type");
  if (!type) return;

  if (*type == "registered") {
    const auto peer_present = json::GetBool(message, "peerPresent");
    LOG_INFO << "registered with broker, client present: "
             << (peer_present.value_or(false) ? "yes" : "no");
    if (peer_present.value_or(false)) {
      std::lock_guard<std::mutex> guard(mutex_);
      client_present_ = true;
      CreateOffer();
    }
    return;
  }

  if (*type == "peer-state") {
    const auto role = json::GetString(message, "role");
    const auto present = json::GetBool(message, "present");
    if (role.value_or("") != "client") return;

    std::lock_guard<std::mutex> guard(mutex_);
    client_present_ = present.value_or(false);
    if (client_present_) {
      LOG_INFO << "client joined - offering";
      CreateOffer();
    } else {
      LOG_INFO << "client left - closing peer";
      // Lift any held input before tearing down, or a key held at the moment
      // the browser closed stays latched on the remote desktop.
      input_->ReleaseAll();
      ClosePeer();
    }
    return;
  }

  if (*type == "renegotiate") {
    LOG_INFO << "client requested renegotiation";
    CreateOffer();
    return;
  }

  if (*type == "answer") {
    const auto sdp = json::GetString(message, "sdp");
    if (sdp) HandleAnswer(*sdp);
    return;
  }

  if (*type == "candidate") {
    HandleRemoteCandidate(message);
    return;
  }

  if (*type == "error") {
    const auto code = json::GetString(message, "code");
    const auto detail = json::GetString(message, "message");
    // no-peer is routine: the host commonly offers before the browser loads.
    if (code.value_or("") == "no-peer") {
      LOG_DEBUG << "broker: no peer in room yet";
    } else {
      LOG_ERROR << "broker error " << code.value_or("?") << ": " << detail.value_or("");
    }
    return;
  }
}

void PeerSession::SendHello() {
  const auto stats = capture_->stats();
  (void)stats;

  std::string hello = "{\"type\":\"hello\",\"hostVersion\":\"" GLSPLAY_VERSION "\"";
  hello += ",\"encoder\":\"" + json::Escape("NVENC H.264 High (P1, ultra-low-latency)") + "\"";
  hello += ",\"gpu\":\"" + json::Escape(capture_->adapter_description()) + "\"";
  hello += ",\"display\":{\"width\":" + std::to_string(capture_->width()) +
           ",\"height\":" + std::to_string(capture_->height()) +
           ",\"refreshHz\":" + std::to_string(config_.video.fps) + "}";
  hello += ",\"gamepadAvailable\":";
  hello += (input_->gamepad() && input_->gamepad()->available()) ? "true" : "false";
  hello += ",\"captureSource\":\"dxgi\"}";

  SendControl(hello);
}

bool PeerSession::SendControl(const std::string& message) {
  if (!control_channel_ ||
      control_channel_->state() != webrtc::DataChannelInterface::kOpen) {
    return false;
  }
  return control_channel_->Send(webrtc::DataBuffer(message));
}

void PeerSession::ClosePeer() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (input_channel_) {
    input_channel_->UnregisterObserver();
    input_channel_->Close();
    input_channel_ = nullptr;
  }
  if (control_channel_) {
    control_channel_->UnregisterObserver();
    control_channel_->Close();
    control_channel_ = nullptr;
  }
  if (peer_) {
    peer_->Close();
    peer_ = nullptr;
  }
}

bool PeerSession::connected() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return peer_ && peer_->peer_connection_state() ==
                      webrtc::PeerConnectionInterface::PeerConnectionState::kConnected;
}

// --- PeerConnectionObserver -------------------------------------------------

void PeerSession::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) {
  LOG_DEBUG << "signaling state " << static_cast<int>(state);
}

void PeerSession::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  // The host creates both channels, so anything arriving here is unexpected.
  LOG_WARN << "unexpected inbound data channel: " << channel->label();
}

void PeerSession::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState state) {
  LOG_DEBUG << "ICE gathering state " << static_cast<int>(state);
}

void PeerSession::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  std::string serialised;
  if (!candidate->ToString(&serialised)) return;

  std::string message = "{\"type\":\"candidate\",\"candidate\":{\"candidate\":\"";
  message += json::Escape(serialised);
  message += "\",\"sdpMid\":\"" + json::Escape(candidate->sdp_mid()) + "\"";
  message += ",\"sdpMLineIndex\":" + std::to_string(candidate->sdp_mline_index()) + "}}";
  send_(message);

  LOG_DEBUG << "local candidate: " << serialised;
}

void PeerSession::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
  using State = webrtc::PeerConnectionInterface::PeerConnectionState;
  switch (state) {
    case State::kConnected:
      LOG_INFO << "peer connected - media flowing";
      SendHello();
      break;
    case State::kDisconnected:
      LOG_WARN << "peer disconnected";
      input_->ReleaseAll();
      break;
    case State::kFailed:
      LOG_ERROR << "peer connection failed - no viable ICE path";
      LOG_ERROR << "Check UDP " << config_.min_port << '-' << config_.max_port
                << " is open on both the VPC and Windows firewalls.";
      input_->ReleaseAll();
      break;
    case State::kClosed:
      LOG_INFO << "peer closed";
      break;
    default:
      break;
  }
}

void PeerSession::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  LOG_DEBUG << "ICE connection state " << static_cast<int>(state);
}

// --- DataChannelObserver ----------------------------------------------------

void PeerSession::OnStateChange() {
  if (input_channel_) {
    LOG_DEBUG << "input channel state " << static_cast<int>(input_channel_->state());
  }
  if (control_channel_ &&
      control_channel_->state() == webrtc::DataChannelInterface::kOpen) {
    SendHello();
  }
}

void PeerSession::OnMessage(const webrtc::DataBuffer& buffer) {
  if (buffer.binary) {
    // Binary is always the input channel.
    input_->HandleMessage(buffer.data.data<uint8_t>(), buffer.data.size());
    return;
  }

  const std::string text(buffer.data.data<char>(), buffer.data.size());
  const auto type = json::GetString(text, "type");
  if (!type) return;

  if (*type == "ping") {
    const auto t_client = json::GetInt(text, "tClient");
    // std::chrono rather than libwebrtc's TimeMillis, which has moved between
    // namespaces and headers across releases.
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    SendControl("{\"type\":\"pong\",\"tClient\":" + std::to_string(t_client.value_or(0)) +
                ",\"tHost\":" + std::to_string(now) + "}");
  } else if (*type == "request-keyframe") {
    // libwebrtc already services RTCP PLI; this is the client asking
    // explicitly, e.g. on tab refocus.
    LOG_DEBUG << "client requested a keyframe";
  } else if (*type == "set-bitrate") {
    const auto kbps = json::GetInt(text, "bitrateKbps");
    if (kbps && *kbps > 0) LOG_INFO << "client requested " << *kbps << " kbps";
  } else if (*type == "set-pointer-mode") {
    // Sent on every Pointer Lock transition. Until this was handled the
    // injector clamped mouselook to the captured monitor, so a sweep past the
    // screen edge injected a zero delta and the camera stopped turning.
    const auto mode = json::GetString(text, "mode");
    if (mode && input_) {
      const bool relative = (*mode == "relative");
      input_->SetPointerRelative(relative);
      LOG_DEBUG << "pointer mode: " << *mode;
    }
  }
}

}  // namespace glsplay
