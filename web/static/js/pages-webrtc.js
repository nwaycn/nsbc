// =============================================================================
// NSBC WebRTC — Vue 组件 + JsSIP 3.10.x
// -----------------------------------------------------------------------------
// 从 nsbc-phone.js 复用：addTrack 幂等 patch / fixVideoPortForLocal /
// sdpCodecWhitelist / PC_CONFIG / ensureVideoReady / tryPlayWithGesture
// Vue 组件自己持有 ua / session / localStream（避免全局状态冲突）
// =============================================================================

/* global JsSIP, app, PC_CONFIG, fixVideoPortForLocal, sdpCodecWhitelist, ensureVideoReady, tryPlayWithGesture */

app.component('webrtc-page', {
  template: `
  <div class="space-y-6">
    <div class="flex items-center gap-3">
      <i class="fa-solid fa-video text-xl text-blue-500"></i>
      <h2 class="text-xl font-semibold">WebRTC 双端互通</h2>
      <span class="text-xs text-gray-400">JsSIP 3.10 + WSS + STUN + nsbcd</span>
    </div>

    <div v-if="!isSecure" class="bg-yellow-50 border border-yellow-200 rounded-lg p-4 text-sm text-yellow-800">
      <i class="fa-solid fa-triangle-exclamation mr-2"></i>
      getUserMedia 需要 HTTPS 或 localhost。当前：<code>{{ location }}</code>
    </div>

    <div class="bg-white rounded-lg border p-5 space-y-4">
      <h3 class="font-medium">SIP 注册 <span class="text-xs text-gray-400 font-normal">· Realm = 租户名</span></h3>
      <div class="grid grid-cols-2 gap-3">
        <div><label class="text-xs text-gray-500">WSS 服务器</label>
          <input v-model="wssServer" class="w-full border rounded px-2 py-1 text-sm" :disabled="registered" /></div>
        <div><label class="text-xs text-gray-500">STUN 服务器</label>
          <input v-model="stunServer" class="w-full border rounded px-2 py-1 text-sm" :disabled="registered" /></div>
        <div><label class="text-xs text-gray-500">分机号 (username)</label>
          <input v-model="username" class="w-full border rounded px-2 py-1 text-sm" :disabled="registered" /></div>
        <div><label class="text-xs text-gray-500">密码</label>
          <input v-model="password" type="password" class="w-full border rounded px-2 py-1 text-sm" :disabled="registered" /></div>
        <div class="col-span-2"><label class="text-xs text-gray-500">Realm / 租户</label>
          <input v-model="realm" @change="onRealmChange"
                 class="w-full border rounded px-2 py-1 text-sm" :disabled="registered"
                 placeholder="nsbc.local · SIP 认证域 / 租户唯一标识" /></div>
      </div>
      <div class="flex gap-2 items-center">
        <button v-if="!registered" @click="doRegister"
                class="px-4 py-1.5 bg-blue-500 text-white rounded text-sm hover:bg-blue-600">
          <i class="fa-solid fa-right-to-bracket mr-1"></i>Register
        </button>
        <button v-else @click="doUnregister"
                class="px-4 py-1.5 bg-red-500 text-white rounded text-sm hover:bg-red-600">
          <i class="fa-solid fa-right-from-bracket mr-1"></i>Unregister
        </button>
        <span class="text-xs" :class="regClass">{{ regStatus }}</span>
      </div>
    </div>

    <div class="bg-white rounded-lg border p-5 space-y-4">
      <h3 class="font-medium">拨号 / 通话</h3>
      <div class="flex gap-2 items-center">
        <input v-model="target" :placeholder="'sip:1002@' + realm"
               class="border rounded px-2 py-1 text-sm flex-1" :disabled="!registered || inCall" />
        <button @click="doCall" :disabled="!registered || (inCall && !pendingSession)"
                class="px-4 py-1.5 bg-green-500 text-white rounded text-sm hover:bg-green-600 disabled:opacity-40">
          <i class="fa-solid fa-phone mr-1"></i>Call
        </button>
        <button @click="doHangup" :disabled="!inCall && !pendingSession"
                class="px-4 py-1.5 bg-red-500 text-white rounded text-sm hover:bg-red-600 disabled:opacity-40">
          <i class="fa-solid fa-phone-slash mr-1"></i>Hangup
        </button>
      </div>
      <div v-if="incomingCall" class="bg-yellow-50 border border-yellow-300 rounded p-3 flex justify-between items-center">
        <span class="text-sm">📞 来电: {{ incomingFrom }}</span>
        <div class="flex gap-2">
          <button @click="doAccept" class="px-3 py-1 bg-green-500 text-white rounded text-sm">Accept</button>
          <button @click="doReject" class="px-3 py-1 bg-red-500 text-white rounded text-sm">Reject</button>
        </div>
      </div>
      <!-- 增强：Hold + DTMF -->
      <div v-if="inCall" class="flex gap-2">
        <button @click="doHold" class="px-3 py-1 text-sm border rounded">
          <i class="fa-solid" :class="held ? 'fa-pause' : 'fa-play'"></i> {{ held ? 'Resume' : 'Hold' }}
        </button>
      </div>
    </div>

    <div class="bg-white rounded-lg border p-5 space-y-4">
      <h3 class="font-medium">媒体</h3>
      <div class="grid grid-cols-2 gap-4">
        <div class="space-y-1">
          <div class="text-xs text-gray-500">本地 <span v-if="localStream?.getVideoTracks?.().length" class="text-green-500">●</span></div>
          <video ref="localVideo" autoplay muted playsinline
                 class="w-full rounded bg-black border" style="height:200px"></video>
        </div>
        <div class="space-y-1">
          <div class="text-xs text-gray-500">远端 <span :class="inCall ? 'text-green-500' : 'text-gray-400'">●</span></div>
          <video ref="remoteVideo" autoplay playsinline
                 class="w-full rounded bg-black border" style="height:200px"></video>
        </div>
      </div>
      <div class="flex gap-2">
        <button @click="toggleAudio" :disabled="!inCall"
                class="px-3 py-1 text-sm border rounded" :class="audioMuted ? 'bg-gray-100' : 'bg-blue-50 text-blue-600'">
          <i class="fa-solid" :class="audioMuted ? 'fa-microphone-slash' : 'fa-microphone'"></i> {{ audioMuted ? 'Unmute' : 'Mute' }}
        </button>
        <button @click="toggleVideo" :disabled="!inCall"
                class="px-3 py-1 text-sm border rounded" :class="videoOff ? 'bg-gray-100' : 'bg-blue-50 text-blue-600'">
          <i class="fa-solid" :class="videoOff ? 'fa-video-slash' : 'fa-video'"></i> {{ videoOff ? 'Start Video' : 'Stop Video' }}
        </button>
      </div>
    </div>

    <div class="bg-white rounded-lg border p-5 space-y-3">
      <h3 class="font-medium">DataChannel</h3>
      <div class="flex gap-2">
        <input v-model="dcMsg" @keyup.enter="dcSend" placeholder="输入消息按回车..."
               class="border rounded px-2 py-1 text-sm flex-1" :disabled="!dcOpen" />
        <button @click="dcSend" :disabled="!dcOpen || !dcMsg.trim()"
                class="px-3 py-1 bg-blue-500 text-white rounded text-sm disabled:opacity-40">Send</button>
      </div>
      <div class="border rounded p-2 text-xs bg-gray-50 h-40 overflow-y-auto space-y-0.5">
        <div v-for="(l, i) in dcLogs" :key="i" :class="l.incoming ? 'text-blue-600' : 'text-gray-600'">
          {{ l.incoming ? '◀' : '▶' }} {{ l.text }}
        </div>
        <div v-if="dcLogs.length === 0" class="text-gray-400">等待 DataChannel open...</div>
      </div>
    </div>

    <div class="bg-white rounded-lg border p-5 space-y-2">
      <h3 class="font-medium">SIP 日志</h3>
      <div ref="sipLog" class="border rounded p-2 text-xs bg-gray-900 text-green-400 h-48 overflow-y-auto font-mono">
        <div v-for="(l, i) in sipLogs" :key="i" v-html="l"></div>
      </div>
    </div>
  </div>
  `,

  data() {
    const host = window.location.hostname || '127.0.0.1';
    return {
      wssServer:  'wss://' + host + ':5062',
      stunServer: 'stun:' + host + ':3478',
      username:   '1001',
      password:   '',
      realm:      localStorage.getItem('nsbc_sip_domain') || 'nsbc.local',
      target:     '',

      ua:         null,
      registered: false,
      regStatus:  '未注册',
      regClass:   'text-gray-400',

      session:        null,   // confirmed 后的 JsSIP session
      pendingSession: null,   // 振铃阶段的 session（呼出/来电未接）
      inCall:         false,  // confirmed = true
      incomingCall:    false,
      incomingFrom:   '',
      held:           false,
      isHangingUp:    false,

      localStream: null,
      remoteStream: null,
      audioMuted: false,
      videoOff: false,

      dc: null,
      dcOpen: false,
      dcMsg: '',
      dcLogs: [],

      sipLogs: [],

      // ======== 铃声 ========
      ringCtx:  null,   // AudioContext（Web Audio API）
      ringNodes: null,  // { osc1, osc2, gain, lfoGain, interval }  —— 正在响铃的节点
      ringPlaying: false,

      location: window.location.origin,
      isSecure: window.location.protocol === 'https:' || window.location.hostname === 'localhost',
    };
  },

  computed: {},

  beforeUnmount() {
    this.stopRing();
    this.doHangup();
    this.doUnregister();
    this.stopMedia();
  },

  methods: {
    onRealmChange() {
      const v = (this.realm || '').trim();
      if (v) {
        try { localStorage.setItem('nsbc_sip_domain', v); } catch(_) {}
        this.pushSipLog(`🔄 Realm = ${v}（租户 = SIP 域）`);
      }
    },

    pushSipLog(html) {
      const ts = new Date().toLocaleTimeString();
      this.sipLogs.push(`<span class="text-gray-500">[${ts}]</span> ${html}`);
      if (this.sipLogs.length > 200) this.sipLogs.shift();
      this.$nextTick(() => {
        const el = this.$refs.sipLog;
        if (el) el.scrollTop = el.scrollHeight;
      });
      const plain = html.replace(/<[^>]*>/g, '');
      console.log(`[NSBC] ${plain}`);
    },
    pushDcLog(text, incoming) {
      this.dcLogs.unshift({ text, incoming });
      if (this.dcLogs.length > 100) this.dcLogs.pop();
    },

    // ======== JsSIP SDP 过滤（Vue 组件自己的 handleSdpEvent）========
    _handleSdpEvent(e) {
      if (!e?.sdp) return;
      if (e.originator !== 'local') return;  // 只过滤本地
      try {
        e.sdp = fixVideoPortForLocal(e.sdp);
        const filtered = sdpCodecWhitelist({ sdp: e.sdp, type: e.type });
        e.sdp = filtered.sdp;
      } catch (err) {
        console.warn('[NSBC] Vue SDP filter failed:', err);
      }
    },

    _makeSessionHandlers(isCallee) {
      const self = this;
      return {
        peerconnection(data) {
          const pc = data.peerconnection;
          self.pushSipLog(`🔗 PC event callee=${isCallee}`);
          // ★ pre-offer addTrack（和 nsbc-phone.js 里一样）
          if (self.localStream) {
            const existingIds = new Set(pc.getSenders().map(s => s.track?.id).filter(Boolean));
            self.localStream.getTracks().forEach(track => {
              if (existingIds.has(track.id)) return;
              try { pc.addTrack(track, self.localStream); } catch(_) {}
            });
          }
          self._attachPC(pc);
        },
      };
    },

    _attachPC(pc) {
      if (!pc) return;
      if (!this.remoteStream) this.remoteStream = new MediaStream();

      pc.ontrack = (e) => {
        if (!this.remoteStream.getTrackById(e.track.id)) this.remoteStream.addTrack(e.track);
        this.$nextTick(() => {
          const rv = this.$refs.remoteVideo;
          if (rv && rv.srcObject !== this.remoteStream) rv.srcObject = this.remoteStream;
          // ★ 强制 unmute——浏览器 autoplay 策略会静默 mute 有声音的视频
          if (rv) {
            rv.muted = false;
            try { rv.unmute && rv.unmute(); } catch(_) {}
          }
          tryPlayWithGesture(rv);
        });
      };
      pc.ondatachannel = (e) => { this.dc = e.channel; this.bindDC(); };
      pc.onconnectionstatechange = () => {
        const st = pc.connectionState;
        if (st === 'connected' || st === 'completed') this.dumpMediaStats(pc, 'connected');
        if (st === 'failed' || st === 'disconnected') this.dumpMediaStats(pc, `fail-${st}`);
      };
      // 呼出侧自己建 DataChannel
      if (this.session && !this.isCallee) {
        try { this.dc = pc.createDataChannel('nsbc-chat'); this.bindDC(); } catch(_) {}
      }

      // ★★★ 来电路径关键兜底（与 nsbc-phone.js 同步）★★★
      // JsSIP answer() 内部先 pc.setRemoteDescription(invite.sdp) → 浏览器触发 ontrack
      // 然后才 emit('peerconnection') → _attachPC 注册 ontrack — 时序反了！
      // 注册完 ontrack 后立即扫一遍已有的 receivers，把漏掉的 track 补回来
      setTimeout(() => {
        const existing = pc.getReceivers().filter(r => r.track).length;
        const ours = this.remoteStream.getTracks().length;
        this.pushSipLog(`🔍 receiver 兜底: receivers=${existing} remoteTracks=${ours} sig=${pc.signalingState}`);
        if (existing > ours) {
          pc.getReceivers().forEach(r => {
            if (r.track && !this.remoteStream.getTrackById(r.track.id)) {
              this.pushSipLog(`🎬 RECOVER: 从 receiver 找回 track kind=${r.track.kind}`);
              this.remoteStream.addTrack(r.track);
            }
          });
          this.$nextTick(() => {
            const rv = this.$refs.remoteVideo;
            if (rv) {
              if (rv.srcObject !== this.remoteStream) rv.srcObject = this.remoteStream;
              rv.muted = false;
              tryPlayWithGesture(rv);
            }
          });
        }
      }, 10);
    },

    _setupLocalPreview(pc) {
      if (!pc) return;
      if (!this.localStream) this.localStream = new MediaStream();
      pc.getSenders().forEach(s => {
        if (s.track && !this.localStream.getTrackById(s.track.id)) {
          this.localStream.addTrack(s.track);
        }
      });
      const lv = this.$refs.localVideo;
      if (this.localStream.getTracks().length > 0 && lv) {
        lv.srcObject = this.localStream;
        try { lv.play().catch(() => {}); } catch(_) {}
      }
    },

    // ======== 注册 ========
    async doRegister() {
      if (!window.JsSIP) { alert('JsSIP 未加载！检查 index.html script src'); return; }
      this.regStatus = '连接中...'; this.regClass = 'text-yellow-600';

      try {
        const socket = new JsSIP.WebSocketInterface(this.wssServer);
        this.ua = new JsSIP.UA({
          sockets: [socket],
          uri: `sip:${this.username}@${this.realm}`,
          password: this.password,
          register: false,
          session_timers: false,
          pcConfig: PC_CONFIG,
          connection_recovery_min_interval: 2,
          connection_recovery_max_interval: 30,
        });

        this.ua.on('connecting',   () => this.pushSipLog(`🔌 JsSIP connecting...`));
        this.ua.on('connected',    () => { this.pushSipLog(`🔌 JsSIP connected`); this.ua.register(); });
        this.ua.on('disconnected', () => this.pushSipLog(`🔌 JsSIP disconnected`));

        this.ua.on('registered', () => {
          this.registered = true;
          this.regStatus = '✅ 已注册 ' + this.username;
          this.regClass = 'text-green-600';
          this.pushSipLog(`<span class="text-green-400">REGISTERED</span> as ${this.username}`);
          // ★ 预热 AudioContext——Register 是用户手势，能绕过 autoplay 策略
          if (!this.ringCtx) {
            try { this.ringCtx = new (window.AudioContext || window.webkitAudioContext)(); } catch(_) {}
          }
          if (this.ringCtx && this.ringCtx.state === 'suspended') {
            this.ringCtx.resume().catch(() => {});
          }
        });
        this.ua.on('unregistered', () => {
          this.registered = false;
          this.regStatus = '未注册';
          this.regClass = 'text-gray-400';
          this.pushSipLog(`<span class="text-gray-400">UNREGISTERED</span>`);
        });
        this.ua.on('registrationFailed', (e) => {
          this.regStatus = '❌ 注册失败';
          this.regClass = 'text-red-500';
          this.pushSipLog(`<span class="text-red-500">REGISTRATION FAILED</span> ${e.cause}`);
        });

        this.ua.on('sip', (e) => {
          try {
            if (e.direction === 'incoming') {
              if (e.response) this.pushSipLog(`← ${e.response.status_code}`);
              else if (e.request) this.pushSipLog(`← ${e.request.method}`);
            } else if (e.direction === 'outgoing') {
              if (e.request) this.pushSipLog(`→ ${e.request.method}`);
            }
          } catch(_) {}
        });

        // ★ 统一 Session 事件绑定（来电路径 + 呼出路径都走这里）
        const self = this;
        this.ua.on('newRTCSession', (e) => {
          const session = e.session;
          const from = session.remote_identity?.uri?.toString() || '?';

          if (e.originator === 'remote') {
            self.pushSipLog(`<span class="text-yellow-400">📞 INCOMING</span> from ${from}`);
            self.pendingSession = session;
            self.incomingCall = true;
            self.incomingFrom = from;
            self.startRing();
            self._bindSession(session, true);
            // ★ 兜底：JsSIP 在 newRTCSession 之前就已 emit peerconnection + setRemoteDescription
            // session.on('peerconnection') 注册太晚 → attachPC 永远不调 → ontrack 丢失
            const incomingPC = session.connection;
            if (incomingPC) {
              self.pushSipLog(`📶 callee: pc 已存在 — 手动 attachPC 兜底`);
              self._attachPC(incomingPC);
            }
          } else {
            self.pushSipLog(`📤 OUTGOING to ${from}`);
            // ★ 呼出也要存 session，振铃阶段能挂断
            self.pendingSession = session;
            self._bindSession(session, false);
          }
        });

        this.ua.start();
      } catch(e) {
        this.regStatus = '❌ 异常';
        this.regClass = 'text-red-500';
        this.pushSipLog(`<span class="text-red-500">EXCEPTION:</span> ${e.message || e}`);
      }
    },

    _bindSession(session, isCallee) {
      session.on('sdp', this._handleSdpEvent);
      session.on('progress', (e) => {
        const r = e.response;
        this.pushSipLog(`📞 progress: ${r?.status_code || ''} ${r?.reason_phrase || ''}`);
      });
      session.on('accepted', () => this.pushSipLog(`✅ accepted (180/183)`));
      session.on('confirmed', () => {
        this.stopRing();
        this.pushSipLog(`<span class="text-green-400">✅ CONFIRMED</span>`);
        this.session = session;
        this.pendingSession = null;
        this.incomingCall = false;
        this.inCall = true;
        this.held = false;
        const pc = session.connection;
        if (pc) {
          this._setupLocalPreview(pc);
          this._attachPC(pc);
        }
      });
      session.on('ended', (e) => {
        this.stopRing();
        const org = e?.originator;
        this.pushSipLog(`📴 ended (originator=${org})`);
        this.onCallEnd(org === 'remote' ? '对端挂断' : '本地挂断');
      });
      session.on('failed', (e) => {
        this.stopRing();
        this.pushSipLog(`<span class="text-red-500">❌ failed:</span> ${e.cause || ''}`);
        this.onCallEnd('失败: ' + (e.cause || ''));
      });
      // ★ 来电路径也要绑 peerconnection——answer() 后 ontrack 在 confirmed 之前就触发，
      // 如果 _attachPC 晚于 ontrack → 远端 track 丢失 → 听不到对端声音。
      session.on('peerconnection', this._makeSessionHandlers(isCallee).peerconnection);
    },

    async doUnregister() {
      try { if (this.ua) this.ua.unregister(); } catch(_) {}
      try { if (this.ua) this.ua.stop(); } catch(_) {}
      this.ua = null;
      this.registered = false;
      this.regStatus = '未注册';
      this.regClass = 'text-gray-400';
    },

    // ======== 呼出 ========
    async doCall() {
      if (!this.ua) return;
      let target = this.target.trim();
      if (!target.startsWith('sip:')) target = 'sip:' + target;
      if (!target.includes('@')) target = target + '@' + this.realm;

      this.pushSipLog(`📤 INVITE → ${target}`);

      try {
        // ★ 先 getUserMedia + ensureVideoReady（和 nsbc-phone.js 一样）
        if (!this.localStream) {
          this.localStream = await navigator.mediaDevices.getUserMedia({ audio: true, video: true });
          await ensureVideoReady(this.localStream);
          const lv = this.$refs.localVideo;
          if (lv) { lv.srcObject = this.localStream; tryPlayWithGesture(lv); }
          this.pushSipLog(`📺 本地流就绪`);
        }
      } catch(e) {
        this.pushSipLog(`<span class="text-red-500">❌ getUserMedia FAIL:</span> ${e.message}`);
        return;
      }

      try {
        // eventHandlers 提前绑定 peerconnection（解决呼出时序坑）
        this.ua.call(target, {
          mediaStream: this.localStream,
          pcConfig: PC_CONFIG,
          eventHandlers: this._makeSessionHandlers(false),
        });
      } catch(e) {
        this.pushSipLog(`<span class="text-red-500">CALL FAIL:</span> ${e.message}`);
      }
    },

    // ======== 接听 ========
    async doAccept() {
      if (!this.pendingSession) return;
      this.stopRing();
      this.pushSipLog(`<span class="text-green-400">✅ ACCEPT</span>`);
      this.incomingCall = false;

      try {
        if (!this.localStream) {
          this.localStream = await navigator.mediaDevices.getUserMedia({ audio: true, video: true });
          await ensureVideoReady(this.localStream);
          const lv = this.$refs.localVideo;
          if (lv) { lv.srcObject = this.localStream; tryPlayWithGesture(lv); }
          this.pushSipLog(`📺 本地流就绪`);
        }
      } catch(e) {
        this.pushSipLog(`<span class="text-red-500">❌ getUserMedia FAIL:</span> ${e.message}`);
        return;
      }

      try {
        this.pendingSession.answer({
          mediaStream: this.localStream,
          pcConfig: PC_CONFIG,
        });
      } catch(e) {
        this.pushSipLog(`<span class="text-red-500">ACCEPT FAIL:</span> ${e.message}`);
      }
    },

    doReject() {
      this.stopRing();
      if (this.pendingSession) {
        try { this.pendingSession.terminate({ status_code: 486, reason_phrase: 'Busy Here' }); } catch(_) {}
        this.pendingSession = null;
      }
      this.incomingCall = false;
      this.pushSipLog(`REJECTED`);
    },

    // ======== 挂断 ========
    doHangup() {
      this.stopRing();
      if (this.isHangingUp) { this.pushSipLog(`⚠️ 挂断进行中`); return; }
      this.isHangingUp = true;

      this.pushSipLog(`📞 HANGUP REQ session=${!!this.session} pending=${!!this.pendingSession}`);
      try {
        const s = this.session || this.pendingSession;
        if (s) s.terminate();
      } catch(e) {
        this.pushSipLog(`<span class="text-red-500">HANGUP FAIL:</span> ${e.message}`);
      }
      // 无论怎样都走清理（JsSIP 的 terminate 是异步的，ended 事件会再次调 onCallEnd——但 Vue 的终止幂等检查已兜底）
      setTimeout(() => { this.onCallEnd('本地挂断'); this.isHangingUp = false; }, 500);
    },

    onCallEnd(reason) {
      this.stopRing();
      if (!this.session && !this.pendingSession && !this.inCall && !this.incomingCall) return;  // 幂等
      this.pushSipLog(`CALL END: ${reason}`);
      this.inCall = false;
      this.held = false;
      this.session = null; this.pendingSession = null;
      this.incomingCall = false; this.incomingFrom = '';
      this.dc = null; this.dcOpen = false;
      this.closePC(); this.stopMedia();
      this.isHangingUp = false;
    },

    // ======== Hold ========
    doHold() {
      const s = this.session;
      if (!s) return;
      try {
        if (this.held) {
          s.unhold(); this.held = false;
          this.pushSipLog(`▶️ UNHOLD`);
        } else {
          s.hold(); this.held = true;
          this.pushSipLog(`⏸ HOLD`);
        }
      } catch(e) {
        this.pushSipLog(`Hold FAIL: ${e.message}`);
      }
    },

    // ======== 媒体 ========
    async dumpMediaStats(pc, tag) {
      try {
        const stats = await pc.getStats();
        let rtpSent = 0, rtpRecv = 0, pktSent = 0, pktRecv = 0, decErr = 0, ploss = 0;
        let dtlsState = '?';
        stats.forEach(s => {
          if (s.type === 'transport') dtlsState = s.dtlsState || '?';
          if (s.type === 'remote-inbound-rtp' || s.type === 'inbound-rtp') {
            rtpRecv += s.bytesReceived || 0; pktRecv += s.packetsReceived || 0;
            decErr += s.decodeErrors || 0; ploss += s.packetsLost || 0;
          }
          if (s.type === 'outbound-rtp') { rtpSent += s.bytesSent || 0; pktSent += s.packetsSent || 0; }
        });
        const media = (rtpSent > 0 || rtpRecv > 0) ? '✅ MEDIA' : '❌ NO MEDIA';
        this.pushSipLog(`📊 STATS[${tag}] ${media} DTLS=${dtlsState}`);
      } catch(e) {
        this.pushSipLog(`📊 STATS[${tag}] fail: ${e.message}`);
      }
    },

    stopMedia() {
      if (this.localStream) {
        this.localStream.getTracks().forEach(t => t.stop());
        this.localStream = null;
      }
      const lv = this.$refs.localVideo; if (lv) lv.srcObject = null;
      const rv = this.$refs.remoteVideo; if (rv) rv.srcObject = null;
      this.remoteStream = null;
    },

    closePC() {
      if (this.dc) { try { this.dc.close(); } catch(_) {} this.dc = null; }
      this.dcOpen = false;
      const pc = this.session?.connection || this.pendingSession?.connection;
      if (pc && pc.signalingState !== 'closed') {
        try { pc.close(); this.pushSipLog(`🛑 PC closed`); } catch(_) {}
      }
    },

    toggleAudio() {
      if (!this.localStream) return;
      this.audioMuted = !this.audioMuted;
      this.localStream.getAudioTracks().forEach(t => t.enabled = !this.audioMuted);
    },
    toggleVideo() {
      if (!this.localStream) return;
      this.videoOff = !this.videoOff;
      this.localStream.getVideoTracks().forEach(t => t.enabled = !this.videoOff);
    },

    // ======== DataChannel ========
    bindDC() {
      if (!this.dc) return;
      this.dc.onopen = () => { this.dcOpen = true; this.pushSipLog(`<span class="text-green-400">DC OPEN</span>`); };
      this.dc.onclose = () => { this.dcOpen = false; this.pushSipLog(`<span class="text-gray-400">DC CLOSED</span>`); };
      this.dc.onmessage = (e) => this.pushDcLog(e.data, true);
      this.dc.onerror = (e) => this.pushSipLog(`<span class="text-red-500">DC ERR:</span> ${e}`);
    },
    dcSend() {
      if (!this.dc || this.dc.readyState !== 'open') return;
      const msg = this.dcMsg.trim();
      if (!msg) return;
      this.dc.send(msg);
      this.pushDcLog(msg, false);
      this.dcMsg = '';
    },

    // ======== 来电铃声（Web Audio API 合成，无需外部 mp3）========
    // 经典两音振铃：440Hz + 480Hz，按 2s 开 / 4s 关循环
    startRing() {
      if (this.ringPlaying) return;
      try {
        if (!this.ringCtx) {
          // AudioContext 在用户手势后才能创建——首次来电时浏览器可能未注册，
          // 先懒创建一次；如果被 autoplay 策略挂起，register 按钮的点击
          // 手势会在下面 resume 兜底。
          try { this.ringCtx = new (window.AudioContext || window.webkitAudioContext)(); }
          catch(_) { /* 不支持 Web Audio API */ return; }
        }
        const ctx = this.ringCtx;
        if (ctx.state === 'suspended') ctx.resume().catch(() => {});

        const gain = ctx.createGain();
        gain.connect(ctx.destination);

        // 440Hz + 480Hz 叠加 → 差频 40Hz 产生颤动感
        const o1 = ctx.createOscillator(); o1.type = 'sine'; o1.frequency.value = 440;
        const o2 = ctx.createOscillator(); o2.type = 'sine'; o2.frequency.value = 480;
        o1.connect(gain); o2.connect(gain);
        o1.start(); o2.start();

        this.ringNodes = { o1, o2, gain, interval: null };
        this.ringPlaying = true;

        // 2s 响 / 4s 静，用定时切 gain.gain 保证占空比精确
        const loop = () => {
          if (!this.ringPlaying) return;
          const now = ctx.currentTime;
          gain.gain.cancelScheduledValues(now);
          gain.gain.setValueAtTime(0.0, now);
          gain.gain.linearRampToValueAtTime(0.4, now + 0.05);
          gain.gain.setValueAtTime(0.4, now + 1.95);
          gain.gain.linearRampToValueAtTime(0.0, now + 2.0);
        };
        loop();
        this.ringNodes.interval = setInterval(loop, 6000);
      } catch(e) {
        console.warn('[NSBC] startRing fail:', e);
      }
    },
    stopRing() {
      if (!this.ringPlaying) return;
      this.ringPlaying = false;
      try {
        if (this.ringNodes) {
          if (this.ringNodes.interval) clearInterval(this.ringNodes.interval);
          try { this.ringNodes.o1?.stop(); } catch(_) {}
          try { this.ringNodes.o2?.stop(); } catch(_) {}
          try { this.ringNodes.lfo?.stop(); } catch(_) {}
          try { this.ringNodes.gain?.disconnect(); } catch(_) {}
          this.ringNodes = null;
        }
      } catch(_) {}
    },
  }
});
