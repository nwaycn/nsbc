/**
 * WebRTC 媒体引擎（与 Android WebRtcMediaEngine 行为对齐）。
 *
 * - 每通呼叫新建 PeerConnection（避免上一通 transceiver 残留）
 * - 先收集完整 ICE candidate 再返回 SDP（最多等 3 秒）
 * - 呼出：addTransceiver('audio'/'video') + 本地轨道
 * - 呼入：远端 offer 不含 m=video 时不添加视频；answer 只使用 offer 中的 transceiver
 * - 通话结束 stop() 会 track.stop() 停止采集并关闭 PeerConnection
 */

const ICE_WAIT_MS = 3000;

export class WebRtcMedia {
  constructor(config, handlers = {}) {
    this.config = { ...config };
    this.onLocalStream = handlers.onLocalStream || null;
    this.onRemoteStream = handlers.onRemoteStream || null;
    this.onError = handlers.onError || null;

    this.pc = null;
    this.localStream = null;
    this.audioTrack = null;
    this.videoTrack = null;
    this.videoSender = null;
    this.remoteStream = null;
    this.remoteHasVideo = false;
    this.videoDeviceId = '';
    this.micEnabled = true;
    this.cameraEnabled = true;
    this.stopped = false;
  }

  iceServers() {
    const servers = [];
    const cfg = this.config;
    if (cfg.stunServer && cfg.stunServer.trim()) {
      servers.push({ urls: cfg.stunServer.trim() });
    }
    if (cfg.turnServer && cfg.turnServer.trim()) {
      servers.push({
        urls: cfg.turnServer.trim(),
        username: cfg.turnUsername || undefined,
        credential: cfg.turnCredential || undefined
      });
    }
    return servers;
  }

  createPeerConnection() {
    const cfg = this.config;
    const pc = new RTCPeerConnection({
      iceServers: this.iceServers(),
      bundlePolicy: 'max-bundle',
      rtcpMuxPolicy: 'require',
      iceCandidatePoolSize: 10,
      iceTransportPolicy: cfg.icePolicy === 'RELAY' ? 'relay' : 'all'
    });

    pc.ontrack = (event) => {
      if (event.streams && event.streams[0]) {
        this.remoteStream = event.streams[0];
      } else {
        if (!this.remoteStream) this.remoteStream = new MediaStream();
        this.remoteStream.addTrack(event.track);
      }
      if (event.track.kind === 'video') this.remoteHasVideo = true;
      if (this.onRemoteStream) this.onRemoteStream(this.remoteStream);
    };

    let iceRestarted = false;
    pc.onconnectionstatechange = () => {
      if (!this.pc) return;
      const state = this.pc.connectionState;
      if (state === 'failed') {
        if (!iceRestarted) {
          iceRestarted = true;
          this.report('媒体连接失败，尝试 restartIce 恢复');
          try {
            this.pc.restartIce();
          } catch (err) {
            this.report(`restartIce 失败: ${err.message}`);
          }
        } else {
          this.report('媒体连接失败，请检查网络或 TURN 配置');
        }
      }
    };

    return pc;
  }

  /** 获取本地媒体；视频失败时自动降级为纯音频（与 Android 摄像头降级一致）。 */
  async prepareLocalMedia(wantVideo) {
    const cfg = this.config;
    const audio = {
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl: true
    };
    const video = {
      width: { ideal: cfg.videoWidth || 1280 },
      height: { ideal: cfg.videoHeight || 720 },
      frameRate: { ideal: cfg.videoFps || 30 },
      facingMode: 'user'
    };

    let stream = null;
    if (wantVideo) {
      try {
        stream = await navigator.mediaDevices.getUserMedia({ audio, video });
      } catch (err) {
        this.report(`摄像头不可用，本次通话使用音频: ${err.message}`);
      }
    }
    if (!stream) {
      try {
        stream = await navigator.mediaDevices.getUserMedia({ audio, video: false });
      } catch (err) {
        throw new Error(`无法获取麦克风: ${err.message}`);
      }
    }

    this.localStream = stream;
    this.audioTrack = stream.getAudioTracks()[0] || null;
    this.videoTrack = stream.getVideoTracks()[0] || null;
    if (this.audioTrack) this.audioTrack.enabled = this.micEnabled;
    if (this.videoTrack) {
      this.videoTrack.enabled = this.cameraEnabled;
      const settings = this.videoTrack.getSettings ? this.videoTrack.getSettings() : {};
      if (settings.deviceId) this.videoDeviceId = settings.deviceId;
    }
    if (this.onLocalStream) this.onLocalStream(stream);
    return stream;
  }

  /** 呼出：创建 offer，等待 ICE 收集完成后返回 SDP。 */
  async createOffer() {
    this.resetPeerConnection();
    this.stopped = false;
    const pc = this.createPeerConnection();
    this.pc = pc;

    await this.prepareLocalMedia(!!this.config.videoEnabled);
    if (this.stopped) throw new Error('媒体已释放');

    const audioTx = pc.addTransceiver('audio', { direction: 'sendrecv' });
    if (this.audioTrack) await audioTx.sender.replaceTrack(this.audioTrack);

    if (this.config.videoEnabled && this.videoTrack) {
      const videoTx = pc.addTransceiver('video', { direction: 'sendrecv' });
      await videoTx.sender.replaceTrack(this.videoTrack);
      this.videoSender = videoTx.sender;
    }

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await this.waitIceComplete(pc);
    if (this.stopped || !pc.localDescription) throw new Error('本地 SDP 为空');
    return filterOfferSdp(pc.localDescription.sdp);
  }

  /** 呼入：应用远端 offer，并按 m-line 挂载本地轨道。 */
  async applyRemoteOffer(sdp) {
    this.resetPeerConnection();
    this.stopped = false;
    this.remoteHasVideo = /(^|\n)m=video\s/.test(String(sdp));
    const wantVideo = !!this.config.videoEnabled && this.remoteHasVideo;

    const pc = this.createPeerConnection();
    this.pc = pc;

    await this.prepareLocalMedia(wantVideo);
    if (this.stopped) throw new Error('媒体已释放');

    await pc.setRemoteDescription({ type: 'offer', sdp });

    // 远端 offer 未含视频时绝不添加视频轨，否则 answer 的 m-line 会多于 offer
    for (const transceiver of pc.getTransceivers()) {
      const kind = transceiver.receiver && transceiver.receiver.track
        ? transceiver.receiver.track.kind
        : null;
      if (kind === 'audio' && this.audioTrack) {
        await transceiver.sender.replaceTrack(this.audioTrack);
        transceiver.direction = 'sendrecv';
      } else if (kind === 'video' && wantVideo && this.videoTrack) {
        await transceiver.sender.replaceTrack(this.videoTrack);
        transceiver.direction = 'sendrecv';
        this.videoSender = transceiver.sender;
      }
    }
  }

  async createAnswer() {
    const pc = this.pc;
    if (!pc) throw new Error('PeerConnection 未创建');
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    await this.waitIceComplete(pc);
    if (this.stopped || !pc.localDescription) throw new Error('本地 SDP 为空');
    return filterAnswerSdp(pc.localDescription.sdp);
  }

  async applyRemoteAnswer(sdp) {
    if (!this.pc) throw new Error('PeerConnection 未创建');
    await this.pc.setRemoteDescription({ type: 'answer', sdp });
  }

  waitIceComplete(pc) {
    if (pc.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((resolve) => {
      let finished = false;
      let timer = null;
      const finish = () => {
        if (finished) return;
        finished = true;
        pc.removeEventListener('icegatheringstatechange', onChange);
        if (timer) clearTimeout(timer);
        resolve();
      };
      const onChange = () => {
        if (pc.iceGatheringState === 'complete') finish();
      };
      pc.addEventListener('icegatheringstatechange', onChange);
      timer = setTimeout(finish, ICE_WAIT_MS);
    });
  }

  setMuted(muted) {
    this.micEnabled = !muted;
    if (this.audioTrack) this.audioTrack.enabled = !muted;
  }

  setVideoEnabled(enabled) {
    this.cameraEnabled = !!enabled;
    if (this.videoTrack) this.videoTrack.enabled = !!enabled;
  }

  /** 切换摄像头（桌面端按 videoinput 设备轮换）。 */
  async switchCamera() {
    if (!this.videoTrack) return false;
    try {
      const devices = await navigator.mediaDevices.enumerateDevices();
      const cameras = devices.filter((d) => d.kind === 'videoinput');
      if (cameras.length < 2) return false;

      const current = cameras.findIndex((d) => d.deviceId === this.videoDeviceId);
      const next = cameras[(current + 1) % cameras.length];
      const stream = await navigator.mediaDevices.getUserMedia({
        video: {
          deviceId: { exact: next.deviceId },
          width: { ideal: this.config.videoWidth || 1280 },
          height: { ideal: this.config.videoHeight || 720 },
          frameRate: { ideal: this.config.videoFps || 30 }
        },
        audio: false
      });
      const track = stream.getVideoTracks()[0];
      if (!track) return false;

      const old = this.videoTrack;
      this.videoTrack = track;
      this.videoDeviceId = next.deviceId;
      track.enabled = this.cameraEnabled;
      if (this.videoSender) await this.videoSender.replaceTrack(track);
      if (this.localStream) {
        this.localStream.removeTrack(old);
        this.localStream.addTrack(track);
      }
      old.stop();
      if (this.onLocalStream) this.onLocalStream(this.localStream);
      return true;
    } catch (err) {
      this.report(`切换摄像头失败: ${err.message}`);
      return false;
    }
  }

  /** 通话结束：停止采集、释放 PeerConnection。 */
  stop() {
    this.stopped = true;
    if (this.localStream) {
      this.localStream.getTracks().forEach((track) => {
        try {
          track.stop();
        } catch {
          // 忽略已停止的轨道
        }
      });
    }
    this.localStream = null;
    this.audioTrack = null;
    this.videoTrack = null;
    this.videoSender = null;
    this.remoteStream = null;
    this.remoteHasVideo = false;

    if (this.pc) {
      try {
        this.pc.ontrack = null;
        this.pc.onconnectionstatechange = null;
        this.pc.close();
      } catch {
        // 忽略关闭异常
      }
    }
    this.pc = null;
  }

  resetPeerConnection() {
    if (this.pc) {
      try {
        this.pc.ontrack = null;
        this.pc.onconnectionstatechange = null;
        this.pc.close();
      } catch {
        // 忽略关闭异常
      }
    }
    this.pc = null;
    this.remoteStream = null;
    this.remoteHasVideo = false;
  }

  report(message) {
    console.warn(`[MEDIA] ${message}`);
    if (this.onError) this.onError(message);
  }
}

// ================================================================ SDP 过滤
// 对齐 Android/iOS：仅 offer 将 H.264 降至 baseline profile (42e01f)，
// 两向仅在缺失时补 packetization-mode=1；Opus 限定采样率 8kHz（maxplaybackrate=8000）。

const H264_BASELINE_PROFILE = '42e01f';

function filterSdp(sdp, isOffer) {
  let result = String(sdp);

  // 1. H.264 fmtp：含 profile-level-id 的行视为 H.264（参数顺序不固定）
  result = result.replace(/^a=fmtp:(\d+)\s([^\r\n]*)$/gm, (match, pt, rest) => {
    if (!/profile-level-id=([0-9a-fA-F]+)/.test(rest)) return match;

    // answer 必须与 offer 的 profile 保持一致，不能改写
    const profileRest = isOffer
      ? rest.replace(/profile-level-id=[0-9a-fA-F]+/, `profile-level-id=${H264_BASELINE_PROFILE}`)
      : rest;

    const finalRest = /packetization-mode/.test(profileRest)
      ? profileRest
      : `${profileRest};packetization-mode=1`;

    return `a=fmtp:${pt} ${finalRest}`;
  });

  // 2. Opus fmtp (pt=111)：补 maxplaybackrate=8000
  result = result.replace(/^a=fmtp:111\s([^\r\n]*)$/gm, (match, rest) => {
    if (/maxplaybackrate=/.test(rest)) {
      return match.replace(/maxplaybackrate=\d+/, 'maxplaybackrate=8000');
    }
    return `a=fmtp:111 ${rest.trim()};maxplaybackrate=8000`;
  });

  // 3. 如果 SDP 里有 m=audio 且 rtpmap 提到 opus (pt=111)，但没有 a=fmtp:111，补一条
  const hasAudio = /^m=audio\s/m.test(result);
  const hasOpusRtpmap = /^a=rtpmap:111\sopus\//m.test(result)
    || /^a=rtpmap:111\sopus\s/m.test(result);
  const hasOpusFmtp = /^a=fmtp:111\s/m.test(result);
  if (hasAudio && hasOpusRtpmap && !hasOpusFmtp) {
    result = result.replace(/^a=rtpmap:111\sopus[^\r\n]*$/m, (match) => {
      return `${match}\r\na=fmtp:111 maxplaybackrate=8000`;
    });
  }

  return result;
}

function filterOfferSdp(sdp) {
  return filterSdp(sdp, true);
}

function filterAnswerSdp(sdp) {
  return filterSdp(sdp, false);
}
