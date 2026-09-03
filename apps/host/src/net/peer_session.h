// Owns the libwebrtc PeerConnection and everything hanging off it.
//
// The host is always the offerer. Making the browser answer-only removes a
// whole class of glare bugs, and the host is the side that knows its encoder
// configuration, so it has strictly more information when composing an offer.

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

#include "config.h"
#include "audio/loopback_audio_device.h"
#include "input/input_dispatcher.h"

namespace glsplay {

class DesktopCaptureSource;
class LoopbackAudioDevice;
class NvencEncoderFactory;

class PeerSession : public webrtc::PeerConnectionObserver,
                    public webrtc::DataChannelObserver {
 public:
  // Emits a JSON string to be sent over the signaling socket.
  using SendSignal = std::function<void(const std::string& json)>;

  PeerSession(const HostConfig& config,
              webrtc::scoped_refptr<DesktopCaptureSource> capture,
              std::shared_ptr<InputDispatcher> input,
              SendSignal send);
  ~PeerSession() override;

  // Builds the PeerConnectionFactory and the encoder factory. Must be called
  // once before any signaling is handled.
  bool Initialise();

  // Handles one JSON message from the broker.
  void HandleSignalingMessage(const std::string& json);

  // Tears down the current peer connection, e.g. when the client disconnects.
  // Takes mutex_; do not call while it is already held - use ClosePeerLocked().
  void ClosePeer();

  // Sends a JSON control message over the reliable "control" channel.
  bool SendControl(const std::string& json);

  bool connected() const;
  NvencEncoderFactory* encoder_factory() const { return encoder_factory_; }

  // --- PeerConnectionObserver --------------------------------------------
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState state) override;
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override;

  // --- DataChannelObserver -----------------------------------------------
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;

 private:
  bool CreatePeerConnection();
  void CreateOffer();
  // Body of ClosePeer(). Assumes the caller already holds mutex_ - the
  // signaling-message handler does when a "client left" arrives, and
  // re-locking a non-recursive std::mutex from the same thread is UB.
  void ClosePeerLocked();
  void HandleAnswer(const std::string& sdp);
  void HandleRemoteCandidate(const std::string& json);
  void SendHello();

  const HostConfig& config_;
  webrtc::scoped_refptr<DesktopCaptureSource> capture_;
  std::shared_ptr<InputDispatcher> input_;
  SendSignal send_;

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_;

  webrtc::scoped_refptr<webrtc::DataChannelInterface> input_channel_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
  webrtc::scoped_refptr<LoopbackAudioDevice> audio_device_;

  // Owned by the factory once handed over; kept for telemetry only.
  NvencEncoderFactory* encoder_factory_ = nullptr;

  mutable std::mutex mutex_;
  bool client_present_ = false;
};

}  // namespace glsplay
