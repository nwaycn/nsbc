// =============================================================================
// NSBC Web — 核心层
// 包含：API wrapper、结构化编辑器、Vue App 创建、CRUD/Live 工厂
// 加载顺序：必须在 vendor/*.js 之后，pages/*.js 之前
// =============================================================================
/* global Vue, ElementPlus, ElementPlusIconsVue */

const { createApp, ref, reactive, computed, onMounted, onBeforeUnmount, watch } = Vue;

// =============================================================================
// NSBC_I18N — full bilingual dictionary (en/zh) + t() helper
// =============================================================================
const NSBC_I18N = {
  en: {
    // --- login ---
    'login.title': 'NSBC Management Console',
    'login.appTitle': 'NSBC',
    'login.username': 'Username',
    'login.password': 'Password',
    'login.captcha': 'Captcha',
    'login.captchaRefresh': 'Click to refresh',
    'login.captchaLoading': 'Loading',
    'login.button': 'Log in',
    'login.buttonLoading': 'Logging in…',
    'login.security': 'Security login',
    'login.version': 'NSBC v2.5.5',
    // --- generic ---
    'generic.cancel': 'Cancel',
    'generic.confirmDelete': 'Confirm delete?',
    'generic.default': 'default',
    'generic.delete': 'Delete',
    'generic.deleteFailed': 'Delete failed',
    'generic.disabled': 'Disabled',
    'generic.edit': 'Edit',
    'generic.enabled': 'Enabled',
    'generic.loading': 'Loading...',
    'generic.mode': 'Mode',
    'generic.name': 'Name',
    'generic.new': 'New',
    'generic.noData': 'No data',
    'generic.refresh': 'Refresh',
    'generic.remarks': 'Remarks',
    'generic.save': 'Save',
    'generic.saveFailed': 'Save failed',
    'generic.search': 'Search',
    'generic.state': 'State',
    'generic.strategy': 'Strategy',
    'generic.type': 'Type',
    'generic.unban': 'Unban',
    'generic.confirmHangupCall': 'Confirm hang up call ',
    // --- menu ---
    'menu.registrations': 'Registrations',
    'menu.routes': 'Routes',
    'menu.timeGroups': 'Time groups',
    // --- tenant ---
    'tenant.title': 'Tenants',
    'tenant.name': 'Tenant name',
    'tenant.matchFrom': 'Match domain',
    'tenant.matchDid': 'Match DID prefix',
    'tenant.matchTrunk': 'Match trunk',
    'tenant.disabled': 'disabled',
    // --- ext ---
    'ext.title': 'Extensions',
    'ext.number': 'Extension',
    'ext.type': 'Type',
    'ext.internal': 'Internal',
    'ext.external': 'External',
    'ext.externalHint': 'Internal = SIP registered extension; External = no SIP registration',
    'ext.externalNumber': 'Outbound number',
    'ext.fullName': 'Full name',
    'ext.outboundTrunk': 'Outbound trunk',
    'ext.callerId': 'Caller ID',
    // --- gateway ---
    'gw.title': 'Gateways',
    // --- trunk ---
    'trunk.title': 'Trunks',
    'trunk.host': 'Host',
    'trunk.port': 'Port',
    'trunk.realm': 'Realm',
    'trunk.authUser': 'Auth user',
    'trunk.authPwd': 'Auth password',
    'trunk.codec': 'Codec set',
    'trunk.weight': 'Weight',
    'trunk.priority': 'Priority',
    'trunk.maxChan': 'Max channels',
    'trunk.perNumMax': 'Max per number',
    'trunk.callerPool': 'Caller ID pool',
    'trunk.proto': 'Protocol',
    // --- sipProfile ---
    'sipProfile.title': 'SIP profiles',
    'sipProfile.name': 'Name',
    'sipProfile.sipIp': 'SIP bind IP',
    'sipProfile.externalIp': 'External SIP IP',
    'sipProfile.udpPort': 'UDP port',
    'sipProfile.tcpPort': 'TCP port',
    'sipProfile.tlsPort': 'TLS port',
    'sipProfile.enableUdp': 'Enable UDP',
    'sipProfile.enableTcp': 'Enable TCP',
    'sipProfile.enableTls': 'Enable TLS',
    'sipProfile.rtpIp': 'RTP bind IP',
    'sipProfile.externalRtpIp': 'External RTP IP',
    'sipProfile.rtpStart': 'RTP start',
    'sipProfile.rtpEnd': 'RTP end',
    'sipProfile.enablePrack': 'Enable PRACK (RFC 3262)',
    'sipProfile.enableUpdate': 'Enable UPDATE (RFC 3311)',
    'sipProfile.dtmfMode': 'DTMF mode',
    // --- queue ---
    'queue.title': 'Queues',
    'queue.member': 'Member',
    'queue.weight': 'Weight',
    'queue.ringStrategy': 'Ring strategy',
    'queue.maxWait': 'Max wait',
    'queue.moh': 'MOH',
    // --- ivr ---
    'ivr.title': 'IVR menus',
    'ivr.keyOption': 'Key option',
    'ivr.target': 'Target',
    'ivr.promptAudio': 'Prompt audio',
    'ivr.invalidAudio': 'Invalid audio',
    'ivr.timeout': 'Timeout',
    'ivr.maxAttempts': 'Max attempts',
    'ivr.timeoutTarget': 'Timeout target',
    'ivr.invalidTarget': 'Invalid target',
    // --- ha ---
    'ha.title': 'HA groups',
    'ha.node': 'Node',
    'ha.quorum': 'Quorum',
    'ha.electionTimeout': 'Election timeout (ms)',
    // --- dp ---
    'dp.title': 'Dialplan rules',
    'dp.reloadC': 'Reload to core',
    'dp.reloadCAfterSave': 'Save and reload',
    'dp.name': 'Rule name',
    'dp.pattern': 'Called match',
    'dp.target': 'Target',
    'dp.priority': 'Priority',
    'dp.profileInternal': 'Internal — SIP profile (registered extensions)',
    'dp.profileExternal': 'External — SIP profile (gateway/DID)',
    'dp.tenant': 'Tenant',
    'dp.mediaMode': 'Media mode',
    'dp.recording': 'Recording',
    'dp.recPathTemplate': 'Recording path template',
    'dp.matchCaller': 'Caller match',
    'dp.stripPrefix': 'Strip prefix',
    'dp.dstOverride': 'Destination override',
    'dp.appChain': 'App chain',
    'dp.actionJson': 'Action JSON',
    'dp.validateExpr': 'Validate regex',
    'dp.exprValid': 'Regex OK',
    'dp.exprInvalid': 'Regex invalid',
    'dp.testNum': 'Test number',
    'dp.canMatch': 'Matches',
    'dp.cannotMatch': 'Does not match',
    // --- rt / ch / reg / cdr ---
    'rt.title': 'Realtime overview',
    'rt.originateTest': 'Originate test call',
    'rt.activeCalls': 'Active calls',
    'rt.totalCalls': 'Total calls',
    'rt.failedCalls': 'Failed calls',
    'rt.registrations': 'Registrations',
    'rt.callUrl': 'Callee / call-url',
    'rt.callerId': 'Caller ID override',
    'rt.callerName': 'Caller name',
    'rt.ringTimeout': 'Ring timeout (s)',
    'rt.app': 'App (optional)',
    'rt.originate': 'Originate',
    'rt.sending': 'Sending...',
    'ch.stateA': 'A state',
    'ch.stateB': 'B state',
    'ch.caller': 'Caller',
    'ch.callee': 'Callee',
    'ch.start': 'Start',
    'ch.answer': 'Answer',
    'ch.duration': 'Duration',
    'ch.codecA': 'A codec',
    'ch.codecB': 'B codec',
    'ch.fromIp': 'Source IP',
    'ch.arip': 'A RTP',
    'ch.brip': 'B RTP',
    'ch.mos': 'MOS',
    'ch.loss': 'Loss',
    'ch.jitter': 'Jitter (ms)',
    'reg.ext': 'Extension',
    'reg.aor': 'AoR',
    'reg.nat': 'NAT',
    'reg.received': 'Received',
    'reg.reachableContact': 'Reachable contact',
    'reg.transport': 'Transport',
    'reg.registered': 'Registered',
    'reg.remaining': 'Remaining',
    'reg.status': 'Status',
    'reg.online': 'Online',
    'reg.soonExpire': 'Expiring soon',
    'reg.noReg': 'No registrations (no extensions registered)',
    'cdr.caller': 'Caller',
    'cdr.callee': 'Callee',
    'cdr.callId': 'Call-ID',
    'cdr.incoming': 'Incoming',
    'cdr.answer': 'Answer',
    'cdr.end': 'End',
    'cdr.duration': 'Duration',
    'cdr.billSec': 'Billing secs',
    'cdr.cause': 'Cause',
    'cdr.download': 'Download',
    // --- live misc ---
    'live.updatedAt': 'Last update',
    'live.originateHint': 'All ESL/api commands take effect in real-time on the core',
    'live.hangup': 'Hang up',
    'live.hangupSent': 'Hang-up command sent',
    'live.hangupFailed': 'Hang-up failed',
    'live.notReg': 'Not registered',
    'live.connecting': 'Connecting...',
    'live.registered': 'Registered',
    'live.registerFailed': 'Registration failed',
    'live.exception': 'Exception',
    'live.remoteEnded': 'Remote party hung up',
    'live.localEnded': 'Hung up',
    'live.failed': 'Failed',
    'live.ratesTitle': 'Rate table',
    'live.ratesUnit': 'Micro-yuan (1e-6 CNY)',
    'live.balancesTitle': 'Account balances',
    'live.balancesUnit': 'Micro-yuan (1e-6 CNY)',
    'live.transcodeTitle': 'Transcode / Media bridge',
    // --- table ---
    'table.actions': 'Actions',
    // --- crud.tenant ---
    'crud.tenant.realm.help': 'SIP auth realm / unique tenant ID, default nsbc.local. Incoming INVITE From domain or extension registration realm must match this.',
    'crud.tenant.matchDomain.help': 'Incoming From domain = this value routes into this tenant (optional)',
    'crud.tenant.matchDid.help': 'Incoming numbers starting with this prefix route into this tenant (optional)',
    'crud.tenant.matchTrunk.help': 'Incoming via this trunk routes into this tenant (optional)',
    // --- crud.common ---
    'crud.common.tenant.help': 'Owning tenant (list auto-filtered by current tenant)',
    // --- crud.ext ---
    'crud.ext.password.help': 'Can be empty for external extensions that do not register',
    'crud.ext.externalNumber.help': 'Actual number to dial outbound',
    'crud.ext.outboundTrunk.help': 'Which trunk to use for outbound (name in Trunks page)',
    'crud.ext.callerId.help': 'Number shown to remote party (empty = trunk default)',
    // --- crud.trunk ---
    'crud.trunk.gateway.help': 'From Groups page (can be empty, meaning no group)',
    'crud.trunk.profile.help': 'From SIP Profiles page (internal=default, external=public)',
    'crud.trunk.kind.label': 'Kind',
    'crud.trunk.kind.staticPeer': 'static_peer',
    'crud.trunk.kind.register': 'register',
    'crud.trunk.kind.authOut': 'auth_out',
    'crud.trunk.fromUser.label': '[FS] From User (empty = auth_user)',
    'crud.trunk.fromDomain.label': 'From Domain (empty = fallback realm->proxy->host)',
    'crud.trunk.proxy.label': 'Outbound proxy (empty = same as host)',
    'crud.trunk.registerProxy.label': 'Register-only proxy (empty = same as proxy)',
    'crud.trunk.expire.label': 'Register expiration (seconds)',
    'crud.trunk.retry.label': 'Retry base (seconds; 0 = no auto-retry)',
    'crud.trunk.callerIdInFrom.label': 'caller_id_in_from (=1 replaces From user part only)',
    'crud.trunk.contactParams.label': 'Contact extra params (e.g. transport=udp)',
    'crud.trunk.extInContact.label': 'extension_in_contact (=1 puts inbound_ext into Contact)',
    'crud.trunk.ping.label': 'OPTIONS health-check interval (0 = disabled)',
    'crud.trunk.inboundExt.label': 'Inbound DID map target (e.g. 2001)',
    'crud.trunk.cidType.label': 'CID type (reserved for rpid etc.)',
    'crud.trunk.rfc5626.label': 'RFC 5626 Outbound (reserved)',
    'crud.trunk.regId.label': 'reg-id (RFC 5626 reserved)',
    'crud.trunk.peerIp.label': 'peer_ip whitelist (CIDR comma-separated, auth_out use)',
    'crud.trunk.imsEnabled.label': 'IMS trunk (auto-injects PAI/PPID/PCPI headers)',
    'crud.trunk.imsDomain.label': 'IMS root domain (e.g. ims.mnc001.mcc460.3gppnetwork.org)',
    'crud.trunk.imsDomain.help': 'Required: once IMS enabled, PAI/PPID/PCPI use {auth_user}@{ims_domain} SIP URI',
    'crud.trunk.imsAutoPani.label': 'Auto-inject P-Access-Network-Info (default E-UTRAN-TDD)',
    'crud.trunk.imsPaniValue.label': 'Custom PANI value (empty = E-UTRAN-TDD)',
    'crud.trunk.imsAutoPrivacy.label': 'Auto-inject Privacy: id (CLIR, remote From anonymous)',
    'crud.trunk.customHeaders.label': 'Custom SIP headers (outbound INVITE)',
    'crud.trunk.customHeaders.help': 'One per line, must end with \\r\\n. Variables: ${callid} ${caller} ${callee} ${from_user} ${to_user} ${from_ip} ${to_ip}',
    // --- crud.route ---
    'crud.route.strategy.priority': 'priority (primary/backup failover)',
    'crud.route.strategy.weight': 'weight (weighted hash)',
    'crud.route.strategy.leastBusy': 'least_busy (least-active first)',
    'crud.route.strategy.ring': 'ring (round-robin)',
    // --- crud.dp ---
    'crud.dp.tenantCol': 'Tenant',
    'crud.dp.recCol': 'Record',
    'crud.dp.recPathCol': 'Recording path template',
    'crud.dp.ipCidrCol': 'IP CIDR allowed',
    'crud.dp.profileLabel': 'SIP Profile',
    'crud.dp.profileHelp': 'Internal profile only matches internal registrations; external profile matches SIP gateway/DID trunks.',
    'crud.dp.tenantHelp': 'Dialplan rule owner tenant.',
    'crud.dp.ipCidrLabel': 'IP CIDR',
    'crud.dp.ipCidrExtSuffix': ' (all 0.0.0.0/0, 0 = public internet)',
    'crud.dp.ipCidrIntSuffix': ' (suggest RFC1918 private CIDR; 0 = fraud risk)',
    'crud.dp.patternLabel': 'Called-party match expression (regex)',
    'crud.dp.patternPlaceholder': 'e.g. ^\\+8610(\\d{4})(\\d{4})$',
    'crud.dp.prioLabel': 'Priority',
    'crud.dp.dstOverrideHelp': 'Override destination. $1 $2 or {1} {2} are capture groups from regex.',
    'crud.dp.targetLabel': 'Target resource (extension / queue / trunk / gateway)',
    'crud.dp.targetPlaceholder': 'e.g. extension:2001 or gateway:OperatorA',
    'crud.dp.noTargetsTip': 'No trunk or gateway available',
    'crud.dp.targetHelp': 'extension:number, queue:name, ivr:name, gateway:name, trunk:name, hangup',
    'crud.dp.actionHelp': 'Advanced custom JSON (e.g. custom headers, recording, media routing)',
    'crud.dp.appChainHelp': 'Application chain executed in order. Leave empty for direct route.',
    'crud.dp.reloadSuccess': 'Reloaded to Core',
    'crud.dp.reloadFail': 'Reload failed',
    'crud.dp.saveFail': 'Save failed',
    'crud.dp.deleteFail': 'Delete failed',
    // --- crud.live ---
    'crud.live.totalFmt': '{total} items · auto-refresh every 10s',
    'crud.live.refresh': 'refresh',
    'crud.live.totalPerPage': '{total} items · {pageSize} per page',
    'crud.live.truncated': 'Output truncated',
    'crud.live.contactRaw': 'Contact (raw)',
    'crud.live.noNat': 'No NAT',
    // --- crud.cdrs ---
    'crud.cdrs.title': 'Call detail records',
    'crud.cdrs.fromIp': 'Source IP',
    'crud.cdrs.codecA': 'A codec',
    'crud.cdrs.codecB': 'B codec',
    'crud.cdrs.ringTime': 'Ring (s)',
    'crud.cdrs.recCol': 'Recording',
    // --- crud.rates ---
    'crud.rates.prefix': 'Prefix',
    'crud.rates.price': 'Price/min (μ)',
    'crud.rates.connectFee': 'Connect fee (μ)',
    'crud.rates.billInc': 'Bill block (s)',
    'crud.rates.minBill': 'Min bill (s)',
    // --- crud.balances ---
    'crud.balances.account': 'Account',
    'crud.balances.balance': 'Balance (μ)',
    'crud.balances.reserved': 'Reserved (μ)',
    'crud.balances.credit': 'Credit limit (μ)',
    'crud.balances.available': 'Available (μ)',
    // --- crud.transcode ---
    'crud.transcode.activeTotal': 'Active',
    'crud.transcode.noSession': 'No active media sessions',
    'crud.transcode.mode': 'Mode',
    'crud.transcode.fwdPkts': 'Forwarded',
    'crud.transcode.dropPkts': 'Dropped',
    // --- sec ---
    'sec.title': 'Security center',
    'sec.dynBan': 'Dynamic ban',
    'sec.searchIpPlaceholder': 'IP (substring, e.g. 66.94)',
    'sec.banHelp': 'Dynamic ban based on auth failure count.',
    'sec.noBan': 'No bans',
    'sec.srcIp': 'Source IP',
    'sec.banned': 'Banned',
    'sec.observing': 'Observing',
    'sec.fails': 'Fails',
    'sec.remaining': 'Remaining',
    'sec.lastActivity': 'Last activity',
    'sec.ago': 'ago',
    'sec.unbanConfirm': 'Unban ',
    'sec.unbanSuccess': 'Unbanned ',
    'sec.unbanFail': 'Unban failed',
    'sec.fence': 'Fence rules',
    'sec.all': 'All',
    'sec.deny': 'deny',
    'sec.allow': 'allow',
    'sec.fenceDenyLabel': 'Blacklist (deny)',
    'sec.fenceAllowLabel': 'Whitelist (allow)',
    'sec.typeCalledPrefix': 'Called prefix',
    'sec.typeCallerPrefix': 'Caller prefix',
    'sec.ruleNew': 'New fence rule',
    'sec.fenceHelp': 'Blacklist checked first (deny on hit). Whitelist checked only in security mode.',
    'sec.noRules': 'No fence rules',
    'sec.mode': 'Mode',
    'sec.type': 'Type',
    'sec.ruleValue': 'Rule value',
    'sec.addTime': 'Added at',
    'sec.deleteRuleConfirm': 'Delete rule ',
    'sec.fenceDenyHit': 'Blacklist (reject on hit)',
    'sec.fenceAllowHit': 'Whitelist (pass on hit)',
    'sec.uaSubstr': 'UA substring',
    'sec.calledPrefix': 'Called prefix',
    'sec.callerPrefix': 'Caller prefix',
    'sec.optional': 'Optional',
    'sec.config': 'Global config',
    'sec.fenceMode': 'Fence mode',
    'sec.fenceModeDesc': 'performance = allow all except deny; security = only allow explicit allow.',
    'sec.blockMethod': 'Disable SIP method',
    'sec.blockMethodDesc': 'Bitmask of SIP methods to block (0 = none).',
    'sec.currentBitmap': 'Current bitmap',
    'sec.reload': 'Reload',
    'sec.configSave': 'Save and push to core',
    'sec.operationFail': ' operation failed',
    'sec.savePushSuccess': 'Saved and pushed to core',
    'sec.enablePushSuccess': 'Enabled and pushed',
    'sec.disablePushSuccess': 'Disabled and pushed',
    'sec.deletePushSuccess': 'Deleted and pushed',
    'sec.pleaseFillValue': 'Please fill in rule value',
    'sec.cfgSavePushSuccess': 'Config saved and pushed to core',
    'sec.phIpCidr': 'e.g. 10.0.0.0/8',
    'sec.phUa': 'e.g. MicroSIP',
    'sec.phCalled': 'e.g. +8610',
    'sec.phCaller': 'e.g. +86138',
    // --- struct ---
    'struct.gwGroupOut': 'Groups — outbound',
    'struct.trunkDirect': 'Trunks — direct route',
    'struct.extBiz': 'Extension / Queue / IVR / Hangup',
    'struct.switchToStruct': 'Switch to structured editor',
    'struct.inputJson': 'Raw JSON object',
    'struct.appChainOrder': 'Apps execute top-down',
    'struct.mediaDescDetail': 'relay=core bridges RTP; bypass=core only forwards signaling',
    'struct.recPathHint': 'Variables: {caller} {callee} {callid} {date} {time}',
    'struct.timeRuleDesc': 'One object per time-range block. days: 1-7',
    'struct.validateError': 'Regex cannot be empty',
    'struct.validateSyntax': 'Syntax error',
    // --- webrtc ---
    'webrtc.title': 'WebRTC dual-end interop',
    'webrtc.securityHint': 'getUserMedia requires HTTPS or localhost. Current:',
    'webrtc.reg': 'Register',
    'webrtc.unreg': 'Unregister',
    'webrtc.realm': 'Realm / tenant',
    'webrtc.realmHint': '= tenant name',
    'webrtc.wssServer': 'WSS server',
    'webrtc.stunServer': 'STUN server',
    'webrtc.extNum': 'Extension (username)',
    'webrtc.password': 'Password',
    'webrtc.realmPh': 'nsbc.local · SIP auth domain / unique tenant identifier',
    'webrtc.callSection': 'Dial / Call',
    'webrtc.sipLog': 'SIP log',
    'webrtc.mediaSection': 'Media',
    'webrtc.incoming': 'Incoming call',
    'webrtc.accept': 'Accept',
    'webrtc.reject': 'Reject',
    'webrtc.hold': 'Hold',
    'webrtc.resume': 'Resume',
    'webrtc.local': 'Local',
    'webrtc.remote': 'Remote',
    'webrtc.mute': 'Mute',
    'webrtc.unmute': 'Unmute',
    'webrtc.stopVideo': 'Stop video',
    'webrtc.startVideo': 'Start video',
    'webrtc.enterMessage': 'Type message, press Enter...',
    'webrtc.waitDataChannel': 'Waiting for DataChannel open...',
    'webrtc.notReg': 'Not registered',
    'webrtc.connecting': 'Connecting...',
    'webrtc.registered': 'Registered',
    'webrtc.registerFailed': 'Registration failed',
    'webrtc.exception': 'Exception',
    'webrtc.call': 'Call',
    'webrtc.hangup': 'Hang up',
    'webrtc.remoteEnded': 'Remote party hung up',
    'webrtc.localEnded': 'Hung up',
    'webrtc.failed': 'Failed',
    // --- generic.more (index.html) ---
    'generic.noDataHint': 'Please select a menu item from the sidebar.',
    'generic.total': 'Total',
    'sidebar.title': 'NSBC Management Console',
    'sidebar.loading': 'Loading...',
    'sidebar.logout': 'Log out',
    'sidebar.selectTenant': 'Select tenant...',
    'sidebar.currentTenant': 'Current tenant',
  },
  zh: {
    // --- login ---
    'login.title': 'NSBC 管理台',
    'login.appTitle': '宁卫SBC',
    'login.username': '用户名',
    'login.password': '密码',
    'login.captcha': '验证码',
    'login.captchaRefresh': '点击刷新验证码',
    'login.captchaLoading': '加载',
    'login.button': '登 录',
    'login.buttonLoading': '登录中…',
    'login.security': '安全登录',
    'login.version': 'NSBC v2.5.5',
    // --- generic ---
    'generic.cancel': '取消',
    'generic.confirmDelete': '确认删除？',
    'generic.default': '默认',
    'generic.delete': '删除',
    'generic.deleteFailed': '删除失败',
    'generic.disabled': '禁用',
    'generic.edit': '编辑',
    'generic.enabled': '启用',
    'generic.loading': '加载中...',
    'generic.mode': '模式',
    'generic.name': '名称',
    'generic.new': '新建',
    'generic.noData': '暂无数据',
    'generic.refresh': '刷新',
    'generic.remarks': '备注',
    'generic.save': '保存',
    'generic.saveFailed': '保存失败',
    'generic.search': '搜索',
    'generic.state': '状态',
    'generic.strategy': '策略',
    'generic.type': '类型',
    'generic.unban': '解封',
    'generic.confirmHangupCall': '确认挂断通话 ',
    // --- menu ---
    'menu.registrations': '注册状态',
    'menu.routes': '路由组',
    'menu.timeGroups': '时间策略组',
    // --- tenant ---
    'tenant.title': '租户',
    'tenant.name': '租户名',
    'tenant.matchFrom': '匹配 From 域',
    'tenant.matchDid': '匹配 DID 前缀',
    'tenant.matchTrunk': '匹配中继',
    'tenant.disabled': '已停用',
    // --- ext ---
    'ext.title': '分机',
    'ext.number': '分机号',
    'ext.type': '类型',
    'ext.internal': '内部',
    'ext.external': '外部',
    'ext.externalHint': '内部 = 注册分机；外部 = 不注册，由 WebRTC/设备发起',
    'ext.externalNumber': '外呼号码',
    'ext.fullName': '全名',
    'ext.outboundTrunk': '外呼中继',
    'ext.callerId': '来电显示',
    // --- gateway ---
    'gw.title': '网关',
    // --- trunk ---
    'trunk.title': '中继',
    'trunk.host': 'Host',
    'trunk.port': 'Port',
    'trunk.realm': 'Realm',
    'trunk.authUser': 'Auth 用户',
    'trunk.authPwd': 'Auth 密码',
    'trunk.codec': '编码集',
    'trunk.weight': '权重',
    'trunk.priority': '优先级',
    'trunk.maxChan': '最大通道',
    'trunk.perNumMax': '单号码上限',
    'trunk.callerPool': '来电号码池',
    'trunk.proto': '协议',
    // --- sipProfile ---
    'sipProfile.title': 'SIP Profile',
    'sipProfile.name': '名称',
    'sipProfile.sipIp': 'SIP 绑定 IP',
    'sipProfile.externalIp': 'SIP NAT 公网 IP',
    'sipProfile.udpPort': 'UDP 端口',
    'sipProfile.tcpPort': 'TCP 端口',
    'sipProfile.tlsPort': 'TLS 端口',
    'sipProfile.enableUdp': '启用 UDP',
    'sipProfile.enableTcp': '启用 TCP',
    'sipProfile.enableTls': '启用 TLS',
    'sipProfile.rtpIp': 'RTP 绑定 IP',
    'sipProfile.externalRtpIp': 'RTP NAT 公网 IP',
    'sipProfile.rtpStart': 'RTP 起始端口',
    'sipProfile.rtpEnd': 'RTP 结束端口',
    'sipProfile.enablePrack': '启用 PRACK (RFC 3262)',
    'sipProfile.enableUpdate': '启用 UPDATE (RFC 3311)',
    'sipProfile.dtmfMode': 'DTMF 模式',
    // --- queue ---
    'queue.title': '分机队列',
    'queue.member': '成员',
    'queue.weight': '权重',
    'queue.ringStrategy': '振铃策略',
    'queue.maxWait': '最大等待',
    'queue.moh': '保持音乐',
    // --- ivr ---
    'ivr.title': 'IVR 菜单',
    'ivr.keyOption': '按键选项',
    'ivr.target': '目标',
    'ivr.promptAudio': '主提示音',
    'ivr.invalidAudio': '无效提示音',
    'ivr.timeout': '超时',
    'ivr.maxAttempts': '最大尝试',
    'ivr.timeoutTarget': '超时跳转',
    'ivr.invalidTarget': '无效跳转',
    // --- ha ---
    'ha.title': '高可用组',
    'ha.node': '节点',
    'ha.quorum': '法定人数',
    'ha.electionTimeout': '选主超时（毫秒）',
    // --- dp ---
    'dp.title': '拨号规则',
    'dp.reloadC': 'Reload 到核心',
    'dp.reloadCAfterSave': '保存并 Reload',
    'dp.name': '规则名称',
    'dp.pattern': '被叫匹配',
    'dp.target': '目标',
    'dp.priority': '优先级',
    'dp.profileInternal': 'internal — 内部 SIP Profile（已注册分机）',
    'dp.profileExternal': 'external — 外部 SIP Profile（SIP 网关/DID 中继）',
    'dp.tenant': '租户',
    'dp.mediaMode': '媒体模式',
    'dp.recording': '录音',
    'dp.recPathTemplate': '录音路径模板',
    'dp.matchCaller': '主叫匹配',
    'dp.stripPrefix': '去前缀',
    'dp.dstOverride': '覆盖目标',
    'dp.appChain': '应用链',
    'dp.actionJson': 'Action JSON',
    'dp.validateExpr': '验证正则',
    'dp.exprValid': '正则 OK',
    'dp.exprInvalid': '正则无效',
    'dp.testNum': '测试号码',
    'dp.canMatch': '可匹配',
    'dp.cannotMatch': '不可匹配',
    // --- rt / ch / reg / cdr ---
    'rt.title': '实时总览',
    'rt.originateTest': '测试呼叫',
    'rt.activeCalls': '在途呼叫',
    'rt.totalCalls': '总呼叫',
    'rt.failedCalls': '失败呼叫',
    'rt.registrations': '注册数',
    'rt.callUrl': '被叫号 / call-url',
    'rt.callerId': '主叫号覆盖',
    'rt.callerName': '主叫显示名',
    'rt.ringTimeout': '振铃超时（秒）',
    'rt.app': '应用（可选）',
    'rt.originate': '发起',
    'rt.sending': '发送中...',
    'ch.stateA': 'A 状态',
    'ch.stateB': 'B 状态',
    'ch.caller': '主叫',
    'ch.callee': '被叫',
    'ch.start': '开始时间',
    'ch.answer': '接通时间',
    'ch.duration': '持续时长',
    'ch.codecA': 'A 编码',
    'ch.codecB': 'B 编码',
    'ch.fromIp': '来源 IP',
    'ch.arip': 'A-RTP',
    'ch.brip': 'B-RTP',
    'ch.mos': 'MOS',
    'ch.loss': '丢包',
    'ch.jitter': '抖动(ms)',
    'reg.ext': '分机',
    'reg.aor': 'AoR',
    'reg.nat': 'NAT',
    'reg.received': 'Received',
    'reg.reachableContact': '可达 Contact',
    'reg.transport': 'Transport',
    'reg.registered': '已注册',
    'reg.remaining': '剩余',
    'reg.status': '状态',
    'reg.online': '在线',
    'reg.soonExpire': '即将过期',
    'reg.noReg': '暂无注册',
    'cdr.caller': '主叫',
    'cdr.callee': '被叫',
    'cdr.callId': 'Call-ID',
    'cdr.incoming': '呼入时间',
    'cdr.answer': '应答时间',
    'cdr.end': '挂机时间',
    'cdr.duration': '接续时长',
    'cdr.billSec': '计费时长',
    'cdr.cause': '处置',
    'cdr.download': '下载',
    // --- live misc ---
    'live.updatedAt': '上次更新',
    'live.originateHint': '所有 ESL/api 命令都会实时生效到核心',
    'live.hangup': '挂断',
    'live.hangupSent': '挂断指令已发送',
    'live.hangupFailed': '挂断失败',
    'live.notReg': '未注册',
    'live.connecting': '连接中...',
    'live.registered': '已注册',
    'live.registerFailed': '注册失败',
    'live.exception': '异常',
    'live.remoteEnded': '对端挂断',
    'live.localEnded': '已挂断',
    'live.failed': '失败',
    'live.ratesTitle': '费率表',
    'live.ratesUnit': '微元 (1e-6 元)',
    'live.balancesTitle': '账户余额',
    'live.balancesUnit': '微元 (1e-6 元)',
    'live.transcodeTitle': '转码 / 媒体桥',
    // --- table ---
    'table.actions': '操作',
    // --- crud.tenant ---
    'crud.tenant.realm.help': 'SIP 认证域 / 租户唯一标识，默认 nsbc.local。呼入 INVITE 的 From 域或分机注册 realm 必须与此值一致。',
    'crud.tenant.matchDomain.help': '呼入 From 域=此值时归入本租户（可选）',
    'crud.tenant.matchDid.help': '呼入号码以此前缀开头时归入本租户（可选）',
    'crud.tenant.matchTrunk.help': '呼入走此中继时归入本租户（可选）',
    // --- crud.common ---
    'crud.common.tenant.help': '所属租户（列表会按当前租户自动过滤）',
    // --- crud.ext ---
    'crud.ext.password.help': '外部分机不注册可留空',
    'crud.ext.externalNumber.help': '外呼拨打的真实号码',
    'crud.ext.outboundTrunk.help': '走哪个 trunk 外呼（Web 中继管理里的名字）',
    'crud.ext.callerId.help': '对端看到的来电号码（空=用 trunk 默认）',
    // --- crud.trunk ---
    'crud.trunk.gateway.help': '从「网关」页面配置的组（可留空）',
    'crud.trunk.profile.help': '从「SIP Profile」页面配置（internal=默认, external=公网）',
    'crud.trunk.kind.label': '类型',
    'crud.trunk.kind.staticPeer': 'static_peer',
    'crud.trunk.kind.register': 'register',
    'crud.trunk.kind.authOut': 'auth_out',
    'crud.trunk.fromUser.label': '[FS] From User（空=auth_user）',
    'crud.trunk.fromDomain.label': 'From Domain（空=走回退链 realm→proxy→host）',
    'crud.trunk.proxy.label': '出局代理（空=同 host）',
    'crud.trunk.registerProxy.label': '注册专用代理（空=同 proxy）',
    'crud.trunk.expire.label': '注册过期时间（秒）',
    'crud.trunk.retry.label': '退避基数（秒，0=FS 不重试）',
    'crud.trunk.callerIdInFrom.label': 'caller_id_in_from（=1 只替换 From user part）',
    'crud.trunk.contactParams.label': 'Contact 额外参数（如 transport=udp）',
    'crud.trunk.extInContact.label': 'extension_in_contact（=1 把 inbound_extension 放入 Contact）',
    'crud.trunk.ping.label': 'OPTIONS 健康探测间隔（0=关闭）',
    'crud.trunk.inboundExt.label': '入站 DID 映射目标（如 2001）',
    'crud.trunk.cidType.label': 'CID 类型（预留）',
    'crud.trunk.rfc5626.label': 'RFC 5626 Outbound（预留）',
    'crud.trunk.regId.label': 'reg-id（预留）',
    'crud.trunk.peerIp.label': 'peer_ip 白名单（CIDR 逗号分隔）',
    'crud.trunk.imsEnabled.label': 'IMS 中继（自动注入 PAI/PPID/PCPI）',
    'crud.trunk.imsDomain.label': 'IMS 根域',
    'crud.trunk.imsDomain.help': '开启 IMS 后，PAI/PPID/PCPI 用 {auth_user}@{ims_domain} 拼 SIP URI',
    'crud.trunk.imsAutoPani.label': '自动注入 PANI',
    'crud.trunk.imsPaniValue.label': '自定义 PANI 值',
    'crud.trunk.imsAutoPrivacy.label': '自动注入 Privacy: id',
    'crud.trunk.customHeaders.label': '自定义 SIP 头',
    'crud.trunk.customHeaders.help': '每行一条，必须带 \\r\\n 结尾。变量：${callid} ${caller} ${callee} ${from_user} ${to_user} ${from_ip} ${to_ip}',
    // --- crud.route ---
    'crud.route.strategy.priority': 'priority (主备优先)',
    'crud.route.strategy.weight': 'weight (加权散列)',
    'crud.route.strategy.leastBusy': 'least_busy (最闲优先)',
    'crud.route.strategy.ring': 'ring (轮询)',
    // --- crud.dp ---
    'crud.dp.tenantCol': '租户',
    'crud.dp.recCol': '录音',
    'crud.dp.recPathCol': '录音路径模板',
    'crud.dp.ipCidrCol': 'IP 准入 CIDR',
    'crud.dp.profileLabel': 'SIP Profile',
    'crud.dp.profileHelp': 'internal 仅匹配内部注册分机；external 匹配 SIP 网关/DID 中继。',
    'crud.dp.tenantHelp': '拨号规则归属租户。',
    'crud.dp.ipCidrLabel': 'IP CIDR',
    'crud.dp.ipCidrExtSuffix': '（全 0.0.0.0/0）',
    'crud.dp.ipCidrIntSuffix': '（建议 RFC1918 私网段）',
    'crud.dp.patternLabel': '被叫匹配正则',
    'crud.dp.patternPlaceholder': 'e.g. ^\\+8610(\\d{4})(\\d{4})$',
    'crud.dp.prioLabel': '优先级',
    'crud.dp.dstOverrideHelp': '覆盖目标。$1 $2 或 {1} {2} 是正则捕获组。',
    'crud.dp.targetLabel': '目标资源',
    'crud.dp.targetPlaceholder': 'e.g. extension:2001',
    'crud.dp.noTargetsTip': '没有可用的 trunk 或 gateway',
    'crud.dp.targetHelp': 'extension:number, queue:name, ivr:name, gateway:name, trunk:name, hangup',
    'crud.dp.actionHelp': '高级自定义 JSON',
    'crud.dp.appChainHelp': '按顺序执行的应用链。为空表示直接路由。',
    'crud.dp.reloadSuccess': '已 Reload 到核心',
    'crud.dp.reloadFail': 'Reload 失败',
    'crud.dp.saveFail': '保存失败',
    'crud.dp.deleteFail': '删除失败',
    // --- crud.live ---
    'crud.live.totalFmt': '共 {total} 条 · 每 10 秒刷新',
    'crud.live.refresh': '刷新',
    'crud.live.totalPerPage': '共 {total} 条 · 每页 {pageSize} 条',
    'crud.live.truncated': '输出被截断',
    'crud.live.contactRaw': 'Contact（原始）',
    'crud.live.noNat': '无 NAT',
    // --- crud.cdrs ---
    'crud.cdrs.title': '通话记录',
    'crud.cdrs.fromIp': '来源 IP',
    'crud.cdrs.codecA': 'A 编码',
    'crud.cdrs.codecB': 'B 编码',
    'crud.cdrs.ringTime': '振铃时长',
    'crud.cdrs.recCol': '录音',
    // --- crud.rates ---
    'crud.rates.prefix': '前缀',
    'crud.rates.price': '单价/分钟(微元)',
    'crud.rates.connectFee': '接通费(微元)',
    'crud.rates.billInc': '计费块(s)',
    'crud.rates.minBill': '最小收费(s)',
    // --- crud.balances ---
    'crud.balances.account': '账户',
    'crud.balances.balance': '余额(微元)',
    'crud.balances.reserved': '已预占(微元)',
    'crud.balances.credit': '透支额度(微元)',
    'crud.balances.available': '可用余额',
    // --- crud.transcode ---
    'crud.transcode.activeTotal': '活跃会话',
    'crud.transcode.noSession': '无活跃媒体会话',
    'crud.transcode.mode': '模式',
    'crud.transcode.fwdPkts': '转发包',
    'crud.transcode.dropPkts': '丢弃包',
    // --- sec ---
    'sec.title': '安全中心',
    'sec.dynBan': '动态封禁',
    'sec.searchIpPlaceholder': 'IP（子串，如 66.94）',
    'sec.banHelp': '基于鉴权失败次数的动态封禁。',
    'sec.noBan': '无封禁',
    'sec.srcIp': '来源 IP',
    'sec.banned': '封禁中',
    'sec.observing': '观察中',
    'sec.fails': '失败次数',
    'sec.remaining': '剩余时间',
    'sec.lastActivity': '最近活动',
    'sec.ago': '前',
    'sec.unbanConfirm': '确认解封 ',
    'sec.unbanSuccess': '已解封 ',
    'sec.unbanFail': '解封失败',
    'sec.fence': '围栏规则',
    'sec.all': '全部',
    'sec.deny': 'deny',
    'sec.allow': 'allow',
    'sec.fenceDenyLabel': '黑名单（deny）',
    'sec.fenceAllowLabel': '白名单（allow）',
    'sec.typeCalledPrefix': '被叫前缀',
    'sec.typeCallerPrefix': '主叫前缀',
    'sec.ruleNew': '新增围栏规则',
    'sec.fenceHelp': '黑名单规则（deny）优先检查。白名单规则仅在安全模式下生效。',
    'sec.noRules': '暂无规则',
    'sec.mode': '模式',
    'sec.type': '类型',
    'sec.ruleValue': '规则值',
    'sec.addTime': '添加时间',
    'sec.deleteRuleConfirm': '确认删除规则 ',
    'sec.fenceDenyHit': '黑名单（命中即丢）',
    'sec.fenceAllowHit': '白名单（命中才放）',
    'sec.uaSubstr': 'UA 子串',
    'sec.calledPrefix': '被叫前缀',
    'sec.callerPrefix': '主叫前缀',
    'sec.optional': '可选',
    'sec.config': '全局配置',
    'sec.fenceMode': '围栏模式',
    'sec.fenceModeDesc': 'performance = 放行所有仅 deny 拦截；security = 仅 allow 放行。',
    'sec.blockMethod': '禁用 SIP Method',
    'sec.blockMethodDesc': '服务端层 SIP Method 禁用位图。',
    'sec.currentBitmap': '当前位图',
    'sec.reload': '重新加载',
    'sec.configSave': '保存并推送核心',
    'sec.operationFail': '操作失败',
    'sec.savePushSuccess': '已保存并推送到核心',
    'sec.enablePushSuccess': '已启用并推送',
    'sec.disablePushSuccess': '已禁用并推送',
    'sec.deletePushSuccess': '已删除并推送',
    'sec.pleaseFillValue': '请填写规则值',
    'sec.cfgSavePushSuccess': '配置已保存并推送到核心',
    'sec.phIpCidr': '如 10.0.0.0/8',
    'sec.phUa': '如 MicroSIP',
    'sec.phCalled': '如 +8610',
    'sec.phCaller': '如 +86138',
    // --- struct ---
    'struct.gwGroupOut': '网关组-出站',
    'struct.trunkDirect': '中继-直连',
    'struct.extBiz': '分机 / 队列 / IVR / 挂断',
    'struct.switchToStruct': '切结构化',
    'struct.inputJson': '输入 JSON',
    'struct.appChainOrder': '应用自上而下执行',
    'struct.mediaDescDetail': 'relay=核心桥接；bypass=仅转发信令',
    'struct.recPathHint': '变量：{caller} {callee} {callid} {date} {time}',
    'struct.timeRuleDesc': '每条一个时间范围块。days 1-7 表示周一到周日',
    'struct.validateError': '正则不能为空',
    'struct.validateSyntax': '语法错误',
    // --- webrtc ---
    'webrtc.title': 'WebRTC 双端互通',
    'webrtc.securityHint': 'getUserMedia 需要 HTTPS 或 localhost。当前：',
    'webrtc.reg': '注册',
    'webrtc.unreg': '注销',
    'webrtc.realm': 'Realm / 租户',
    'webrtc.realmHint': '= 租户名',
    'webrtc.wssServer': 'WSS 服务器',
    'webrtc.stunServer': 'STUN 服务器',
    'webrtc.extNum': '分机号（username）',
    'webrtc.password': '密码',
    'webrtc.realmPh': 'nsbc.local · SIP 认证域',
    'webrtc.callSection': '拨号 / 通话',
    'webrtc.sipLog': 'SIP 日志',
    'webrtc.mediaSection': '媒体',
    'webrtc.incoming': '来电',
    'webrtc.accept': '接听',
    'webrtc.reject': '拒接',
    'webrtc.hold': '保持',
    'webrtc.resume': '恢复',
    'webrtc.local': '本地',
    'webrtc.remote': '远端',
    'webrtc.mute': '静音',
    'webrtc.unmute': '取消静音',
    'webrtc.stopVideo': '关闭视频',
    'webrtc.startVideo': '开启视频',
    'webrtc.enterMessage': '输入消息按回车...',
    'webrtc.waitDataChannel': '等待 DataChannel open...',
    'webrtc.notReg': '未注册',
    'webrtc.connecting': '连接中...',
    'webrtc.registered': '已注册',
    'webrtc.registerFailed': '注册失败',
    'webrtc.exception': '异常',
    'webrtc.call': '呼叫',
    'webrtc.hangup': '挂断',
    'webrtc.remoteEnded': '对端挂断',
    'webrtc.localEnded': '已挂断',
    'webrtc.failed': '失败',
    // --- generic.more ---
    'generic.noDataHint': '请从左侧边栏选择菜单。',
    'generic.total': '总数',
    'sidebar.title': 'NSBC 管理台',
    'sidebar.loading': '加载中...',
    'sidebar.logout': '登出',
    'sidebar.selectTenant': '选择租户...',
    'sidebar.currentTenant': '当前租户',
  },
};

function t(key, params) {
  const lang = localStorage.getItem('nsbc_lang') || 'en';
  let d = (NSBC_I18N[lang] && NSBC_I18N[lang][key]) || NSBC_I18N.en[key];
  if (d == null) return key;
  if (params && typeof params === 'object') {
    for (const k in params) {
      d = String(d).split('{' + k + '}').join(params[k]);
    }
  }
  return d;
}
window.t = t;
window.NSBC_I18N = NSBC_I18N;

// ===== API wrapper =====
const API = {
  base: '/api/v1',
  token: () => localStorage.getItem('nsbc_token'),
  tenantId: () => parseInt(localStorage.getItem('nsbc_tenant_id') || '1', 10) || 1,
  headers(hasBody) {
    const h = { 'x-nsbc-api-version': '1' };
    if (hasBody) h['Content-Type'] = 'application/json';
    if (this.token()) h['Authorization'] = 'Bearer ' + this.token();
    return h;
  },
  async req(method, path, body) {
    const hasBody = body !== undefined;
    const opts = { method, headers: this.headers(hasBody) };
    if (hasBody) opts.body = JSON.stringify(body);
    // GET 请求自动拼 tenant_id（tenant_id=0 或空表示不过滤）
    // 排除 /tenants 本身（租户元数据源，不能被 tenant_id 过滤）
    let finalPath = path;
    if (!hasBody && method === 'GET' && !path.startsWith('/tenants')) {
      const tid = this.tenantId();
      if (tid > 0) {
        const sep = path.includes('?') ? '&' : '?';
        finalPath = path + sep + 'tenant_id=' + tid;
      }
    }
    const res = await fetch(this.base + finalPath, opts);
    if (res.status === 401) {
      // === Session 缓存：自动用 refresh_token 续期 ===
      const refreshToken = localStorage.getItem('nsbc_refresh_token');
      const isRefreshReq = path === '/auth/refresh';
      if (refreshToken && !isRefreshReq && !this._refreshing) {
        try {
          this._refreshing = true;
          const refreshRes = await fetch(this.base + '/auth/refresh', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ refresh_token: refreshToken }),
          });
          if (refreshRes.ok) {
            const rd = await refreshRes.json();
            localStorage.setItem('nsbc_token', rd.access_token);
            if (rd.refresh_token) localStorage.setItem('nsbc_refresh_token', rd.refresh_token);
            try { window.__auth && (window.__auth.token = rd.access_token); } catch(_) {}
            // 重放原请求（token 已更新）
            const retryRes = await fetch(this.base + path, opts);
            if (retryRes.status === 401) {
              // 还是 401 → 真没救了
              localStorage.clear();
              location.hash = '#/login';
              throw new Error('未授权');
            }
            const retryText = await retryRes.text();
            if (!retryRes.ok) throw new Error(retryText || retryRes.statusText);
            return retryText ? JSON.parse(retryText) : null;
          }
        } catch (refreshErr) { /* refresh 失败走下面清 localStorage */ }
        finally { this._refreshing = false; }
      }
      // refresh 也失败 → 清 session 跳登录
      localStorage.clear();
      try { window.__auth && (window.__auth.token = null, window.__auth.user = null, window.__auth.role = null); } catch(_) {}
      location.hash = '#/login';
      throw new Error('未授权');
    }
    const text = await res.text();
    const data = text ? JSON.parse(text) : null;
    if (!res.ok) throw new Error((data && data.detail) || res.statusText);
    return data;
  },
  get:    (p) => API.req('GET', p),
  post:   (p, b) => API.req('POST', p, b),
  put:    (p, b) => API.req('PUT', p, b),
  delete: (p) => API.req('DELETE', p),
};

// ===== 结构化编辑器 =====

// 1) 时间段（time_ranges → JSON 数组）
const TimeRangeEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">时间段规则（{{ rows.length }} 条）</span>
        <span class="text-gray-400 text-[11px]">days: 1-5 / 1,3,5 / 0-6</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无时间段</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.days" placeholder="1-5" class="w-28">
        <span class="text-gray-400 text-xs">起</span>
        <input v-model="row.from" placeholder="09:00" class="w-24">
        <span class="text-gray-400 text-xs">止</span>
        <input v-model="row.to" placeholder="18:00" class="w-24">
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({days:'1-5',from:'09:00',to:'18:00'})"><i class="fa-solid fa-plus mr-1"></i>添加时间段</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({days:'1-5',from:'09:00',to:'18:00'}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 2) 队列成员（members_json → JSON 数组）
const QueueMembersEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">队列成员（{{ rows.length }} 人）</span>
        <span class="text-gray-400 text-[11px]">extension / weight(1-100) / enabled</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无成员</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.extension" placeholder="分机号 2000" class="w-32">
        <input v-model.number="row.weight" type="number" min="1" max="100" placeholder="权重" class="w-20">
        <label class="flex items-center gap-1 text-xs">
          <input type="checkbox" v-model="row.enabled"> 启用
        </label>
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({extension:'',weight:10,enabled:true})"><i class="fa-solid fa-plus mr-1"></i>添加成员</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({extension:'',weight:10,enabled:true}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 3) IVR 按键选项（options_json → JSON 数组）
const IvrOptionsEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">按键选项（{{ rows.length }} 项）</span>
        <span class="text-gray-400 text-[11px]">key=0-9/*/#  target=extension/queue/ivr/vm</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无选项</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <select v-model="row.key" class="w-20">
          <option v-for="k in ['0','1','2','3','4','5','6','7','8','9','*','#']" :key="k" :value="k">{{ k }}</option>
        </select>
        <span class="text-gray-400">→</span>
        <input v-model="row.target" placeholder="目标" class="flex-1">
        <i class="fa-solid fa-trash struct-del ml-2" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({key:'1',target:''})"><i class="fa-solid fa-plus mr-1"></i>添加按键选项</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() { _s = true; rows.value = JSON.parse(props.modelValue || '[]'); if (!rows.value.length) rows.value.push({key:'1',target:''}); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// 4) Trunk Codec Set（逗号分隔字符串 "PCMA,PCMU,G729"）
const TRUNK_CODECS = ['PCMA','PCMU','G711U','G711A','G729','G722','G722.2','ILBC','OPUS','SPEEX','GSM','AMR','AMR-WB'];
const TrunkCodecEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">可用编码（{{ selected.length }} 个）</span>
        <span class="text-gray-400 text-[11px]">勾选自动生成逗号串</span>
      </div>
      <div class="codec-grid">
        <label v-for="c in TRUNK_CODECS" :key="c">
          <input type="checkbox" :value="c" v-model="selected"> {{ c }}
        </label>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-xs text-gray-600 truncate">{{ modelValue || '未选择' }}</span>
        <button class="text-xs text-blue-600 hover:text-blue-800" @click="selectAll(true)">全选</button>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const selected = ref([]);
    let _s = false;
    function syncFrom() { _s = true; selected.value = (props.modelValue || '').split(',').map(s=>s.trim()).filter(Boolean); _s = false; }
    function syncTo()  { if (_s) return; emit('update:modelValue', selected.value.join(',')); }
    function selectAll(v) { selected.value = v ? [...TRUNK_CODECS] : []; }
    watch(() => props.modelValue, syncFrom);
    watch(selected, syncTo, { deep: true });
    syncFrom();
    return { selected, TRUNK_CODECS, selectAll };
  },
};

// 5) Dialplan Action（action_json → JSON 对象，字段名与 C 侧 sip_hdr_rewrite_t 完全对齐）
const DialplanActionEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">拨号动作（dp_rule 附加字段）</span>
        <span class="text-gray-400 text-[11px]">号码变换 / SIP 头改写 / 录音控制</span>
      </div>
      <div class="px-3 py-3 space-y-2.5 bg-white max-h-[600px] overflow-y-auto">
        <!-- 号码变换 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-0.5">─ 号码变换 ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">clip 强制 CLIP</span>
          <input v-model="action.clip" placeholder="空=不改写，如 442071234567">
        </div>
        <div class="struct-row">
          <label class="inline-flex items-center gap-1.5">
            <input type="checkbox" v-model="action.immediate" class="flex-none">
            <span class="text-gray-600 text-xs">immediate — DigitMap 收齐即匹配</span>
          </label>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">perm 出局权限</span>
          <input v-model.number="action.perm" type="number" min="0" max="255" class="w-20">
          <span class="hint">(0-255)</span>
        </div>

        <!-- From 头 -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ From 头（从低到高优先级） ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_from_display 显示名</span>
          <input v-model="action.sip_from_display" placeholder='如 "Sales Team"'>
          <span class="hint">组件级</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_from_uri URI 部分</span>
          <input v-model="action.sip_from_uri" placeholder="如 sip:alice@nsbc.local">
          <span class="hint">组件级</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from 完整From</span>
          <input v-model="action.sip_invite_from" placeholder='完整 From 值（最高优先级）'>
          <span class="hint">覆盖全部</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_uri</span>
          <input v-model="action.sip_invite_from_uri" placeholder='纯 URI 覆盖，如 sip:alice@nsbc.local'>
          <span class="hint">FS 对齐</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_from_params</span>
          <input v-model="action.sip_invite_from_params" placeholder='如 transport=tls'>
          <span class="hint">From params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_number</span>
          <input v-model="action.origination_caller_id_number" placeholder='FreeSWITCH 精细化主叫 — 只填 user part'>
          <span class="hint">FS 对齐</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">orig_caller_id_name</span>
          <input v-model="action.origination_caller_id_name" placeholder='Display name（空=同 number）'>
          <span class="hint">FS 对齐</span>
        </div>

        <!-- To / R-URI / Contact -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ To / R-URI / Contact ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_to_uri To覆盖</span>
          <input v-model="action.sip_invite_to_uri" placeholder="空=透传，如 sip:bob@other.com">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_to_params</span>
          <input v-model="action.sip_invite_to_params" placeholder="如 user=phone">
          <span class="hint">To params</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_req_uri R-URI</span>
          <input v-model="action.sip_invite_req_uri" placeholder="完整 Request-URI 覆盖">
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_tel_params</span>
          <input v-model="action.sip_invite_tel_params" placeholder="tel: URI params">
          <span class="hint">tel: 追加</span>
        </div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_contact_params</span>
          <input v-model="action.sip_invite_contact_params" placeholder="如 transport=tcp">
          <span class="hint">Contact params</span>
        </div>

        <!-- Domain override -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ Domain override ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_domain 强制域</span>
          <input v-model="action.sip_invite_domain" placeholder="如 sip.example.com">
          <span class="hint">全局 host 替换</span>
        </div>

        <!-- Extra headers -->
        <div class="text-[11px] text-gray-500 font-semibold pt-2">─ 额外 SIP 头（注入 extra 块） ─</div>
        <div class="struct-row">
          <span class="struct-label w-40 shrink-0">sip_invite_pid_params</span>
          <input v-model="action.sip_invite_pid_params" placeholder="如 P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>">
          <span class="hint">完整头行或简写</span>
        </div>
        <div class="struct-row flex-wrap items-start">
          <span class="struct-label w-40 shrink-0 self-stretch">sip_invite_params 多块</span>
          <div class="flex-1 min-w-0">
            <textarea v-model="action.sip_invite_params" rows="4"
              placeholder="每行一个完整头（C 侧自动拼接 \r\n），例如：&#10;P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001&#10;P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>&#10;Privacy: id"
              class="w-full text-xs"></textarea>
            <div class="flex items-center gap-2 mt-1">
              <span class="text-[11px] text-gray-400">快速插入：</span>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tpl3gpp">3GPP IMS（PAI+PPID+PANI+PCPI）</button>
              <button type="button" class="inline-block text-[11px] px-2 py-0.5 bg-slate-100 border border-slate-300 rounded hover:bg-slate-200" @click="tplClir">运营商 CLIR（Privacy+PAI）</button>
            </div>
          </div>
          <span class="hint">extra 头块</span>
        </div>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-[11px] text-gray-500 truncate">JSON: {{ preview }}</span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    // 所有 action_json 字段与 C 侧 sip_hdr_rewrite_t 同名
    const action = reactive({
      // 号码变换
      dst_override: '', clip: '', perm: 0, immediate: false,
      // From 头
      sip_invite_from: '', sip_invite_from_uri: '', sip_from_uri: '', sip_from_display: '', sip_invite_from_params: '',
      // FS 精细化主叫（只填 user part，host 自动走 trunk.from_domain）
      origination_caller_id_number: '', origination_caller_id_name: '',
      // To / R-URI / Contact
      sip_invite_to_uri: '', sip_invite_to_params: '',
      sip_invite_req_uri: '', sip_invite_tel_params: '',
      sip_invite_contact_params: '',
      // Domain
      sip_invite_domain: '',
      // Extra
      sip_invite_params: '', sip_invite_pid_params: '',
      // legacy 别名（兼容旧 JSON）
      clip_from: '',
    });
    let _s = false;
    const preview = computed(() => JSON.stringify(action));
    function syncFrom() {
      _s = true;
      try {
        const v = JSON.parse(props.modelValue || '{}');
        // 号码变换
        action.dst_override = v.dst_override ?? '';
        action.clip = v.clip ?? '';
        action.perm = Number(v.perm ?? 0);
        action.immediate = !!v.immediate;
        // From 头
        action.sip_invite_from = v.sip_invite_from ?? '';
        action.sip_invite_from_uri = v.sip_invite_from_uri ?? '';
        action.sip_from_uri = v.sip_from_uri ?? '';
        action.sip_from_display = v.sip_from_display ?? '';
        action.sip_invite_from_params = v.sip_invite_from_params ?? '';
        // FS 精细化主叫
        action.origination_caller_id_number = v.origination_caller_id_number ?? '';
        action.origination_caller_id_name = v.origination_caller_id_name ?? '';
        // To / R-URI / Contact
        action.sip_invite_to_uri = v.sip_invite_to_uri ?? '';
        action.sip_invite_to_params = v.sip_invite_to_params ?? '';
        action.sip_invite_req_uri = v.sip_invite_req_uri ?? '';
        action.sip_invite_tel_params = v.sip_invite_tel_params ?? '';
        action.sip_invite_contact_params = v.sip_invite_contact_params ?? '';
        // Domain
        action.sip_invite_domain = v.sip_invite_domain ?? '';
        // Extra
        action.sip_invite_params = v.sip_invite_params ?? '';
        action.sip_invite_pid_params = v.sip_invite_pid_params ?? '';
        // legacy
        action.clip_from = v.clip_from ?? '';
      } catch {}
      _s = false;
    }
    function syncTo()  { if (_s) return; emit('update:modelValue', JSON.stringify(action)); }
    watch(() => props.modelValue, syncFrom);
    watch(action, syncTo, { deep: true });
    syncFrom();
    // —— 快速模板注入 ——
    // 3GPP IMS 运营商常用头：PAI（主叫身份）、PPID（首选主叫）、
    // PANI（接入网类型）、PCPI（被叫 E.164 标识）、Privacy（CLIR）
    function tpl3gpp() {
      action.sip_invite_pid_params = 'P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>';
      action.sip_invite_params =
`P-Preferred-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>
P-Access-Network-Info: E-UTRAN-TDD; utran-cell-id-3gpp=0001001
P-Called-Party-ID: <sip:+8613900139000@ims.mnc001.mcc460.3gppnetwork.org>
Privacy: id`;
    }
    // 运营商 CLIR：主叫号码隐藏（Privacy: id 触发 B 腿 From anonymize）
    // + 带 P-Asserted-Identity 让对端网络仍能拿到真实主叫用于计费/路由
    function tplClir() {
      action.sip_invite_pid_params = 'P-Asserted-Identity: <sip:+8613800138000@ims.mnc001.mcc460.3gppnetwork.org>';
      action.sip_invite_params =
`Privacy: id`;
    }
    return { action, preview, tpl3gpp, tplClir };
  },
};

// 6) AppChainEditor —— Application 链（REQ-DP-APP-001）
// C 侧 dp_json_parse_one 解析 top-level "app_chain": [{"app":"playback","data":"welcome.wav"}, ...]
const AppChainEditor = {
  props: ['modelValue'],
  template: `
    <div class="app-chain-editor">
      <div class="struct-header">
        <span class="font-semibold">拨号动作链（Application Chain）</span>
        <span class="text-gray-400 text-[11px]">按顺序执行：pre_answer → playback → sleep → answer …</span>
      </div>
      <div class="px-3 py-3 bg-white space-y-2">
        <!-- 添加动作 -->
        <div class="flex items-center gap-2">
          <el-select v-model="nextApp" placeholder="选择要添加的应用…" style="flex:1" size="small" filterable>
            <el-option-group v-for="g in appGroups" :key="g.label" :label="g.label">
              <el-option v-for="a in g.apps" :key="a.name" :value="a.name" :label="a.name + ' — ' + a.desc">
                <div class="flex justify-between items-center w-full">
                  <span class="font-mono text-xs">{{ a.name }}</span>
                  <span class="text-gray-400 text-[11px]">{{ a.desc }}</span>
                </div>
              </el-option>
            </el-option-group>
          </el-select>
          <el-button size="small" type="primary" :disabled="!nextApp" @click="addApp">
            <el-icon><Plus /></el-icon>添加
          </el-button>
        </div>

        <!-- 空状态 -->
        <div v-if="!chain.length" class="text-center py-4 text-gray-400 text-xs">
          暂无拨号动作。上方选一个 app 添加进来，按顺序执行。
        </div>

        <!-- 链列表 -->
        <div v-for="(item, idx) in chain" :key="idx"
             class="flex items-start gap-2 p-2 bg-slate-50 border border-slate-200 rounded">
          <span class="flex-none w-6 h-6 flex items-center justify-center bg-blue-500 text-white text-xs rounded-full font-mono mt-0.5">{{ idx + 1 }}</span>
          <div class="flex-1 min-w-0 space-y-1">
            <div class="flex items-center gap-2">
              <el-tag size="small" type="info" effect="plain" class="font-mono">{{ item.app }}</el-tag>
              <span class="text-[11px] text-slate-500">{{ appHint(item.app) }}</span>
            </div>
            <el-input v-if="needsData(item.app)" v-model="item.data" size="small"
                      :placeholder="dataPlaceholder(item.app)" clearable></el-input>
          </div>
          <div class="flex-none flex flex-col gap-0.5">
            <el-button link size="small" :disabled="idx === 0" @click="moveUp(idx)">
              <el-icon><ArrowUp /></el-icon>
            </el-button>
            <el-button link size="small" :disabled="idx === chain.length - 1" @click="moveDown(idx)">
              <el-icon><ArrowDown /></el-icon>
            </el-button>
            <el-button link size="small" type="danger" @click="removeAt(idx)">
              <el-icon><Delete /></el-icon>
            </el-button>
          </div>
        </div>
      </div>
      <div class="struct-header border-t bg-white">
        <span class="font-mono text-[11px] text-gray-500 truncate">
          JSON: {{ chain.length ? preview : '[] (空链，C 侧跳过)' }}
        </span>
      </div>
    </div>
  `,
  setup(props, { emit }) {
    const { ref, computed, watch } = Vue;
    const { Plus, ArrowUp, ArrowDown, Delete } = ElementPlusIconsVue || {};

    /* C 侧 app_registry.c builtin_apps[] 注册的 17 个内建 apps（必须对齐） */
    const APP_DESCS = {
      answer:              '应答来电（发送 200 OK）',
      pre_answer:          'Early Media 预应答（发 183，不发 200 OK）',
      bridge:              '桥接到 sofia/gateway/<gw>/<ext>',
      playback:            '播放单个音频文件',
      post_playback:       '接通后放音（阶段区分用）',
      record:              '录制当前通话音频',
      transfer:            '转移当前通话（REFER 或 302）',
      set:                 '设置通道变量 var=value',
      conference:          '加入会议 <room>@<profile>',
      play_and_get_digits: '播放提示音并收集 DTMF',
      sleep:               '暂停执行（毫秒）',
      park:                '保持通话等待取回（hold + MoH）',
      att_xfer:            '出席/咨询转移',
      gentones:            '生成特定音调序列',
      endless_playback:    '循环播放（uuid_break 可中断）',
      uuid_break:          '打断 playback/sleep',
      read:                'DTMF 收号',
    };

    /* 按执行阶段分组（引导用户构建合理顺序） */
    const appGroups = computed(() => [
      { label: '— Early Media / 应答阶段 —', apps: ['pre_answer', 'playback', 'post_playback', 'sleep', 'answer', 'endless_playback', 'read', 'play_and_get_digits'] },
      { label: '— 路由/桥接阶段 —',        apps: ['bridge', 'transfer', 'att_xfer', 'conference', 'park'] },
      { label: '— 变量/控制 —',            apps: ['set', 'uuid_break', 'gentones'] },
      { label: '— 媒体/录音 —',            apps: ['record'] },
    ].map(g => ({
      label: g.label,
      apps: g.apps.map(n => ({ name: n, desc: APP_DESCS[n] || '' })),
    })));

    const nextApp = ref('');
    const chain = ref([]);

    function appHint(name)  { return APP_DESCS[name] || ''; }
    function needsData(name){ return ['playback','post_playback','endless_playback','sleep','bridge','transfer','conference','set','gentones','record','play_and_get_digits','read'].includes(name); }
    function dataPlaceholder(name) {
      const H = {
        playback:             'welcome.wav 或 /path/to/file.wav',
        post_playback:        'announce.wav',
        endless_playback:     'hold_music.wav',
        sleep:                '1500 (毫秒)',
        bridge:               'sofia/gateway/my_trunk/8613800138000',
        transfer:             'sip:support@other.com',
        conference:           'my_room@default',
        set:                  'var_name=value',
        gentones:             '1004,1500,800',
        record:               '/var/recording/callid.wav',
        play_and_get_digits:  'prompt.wav 3 5000',
        read:                 '5 10000',
      };
      return H[name] || '(无 data)';
    }

    function addApp() {
      if (!nextApp.value) return;
      chain.value.push({ app: nextApp.value, data: '' });
      nextApp.value = '';
    }
    function removeAt(i)  { chain.value.splice(i, 1); }
    function moveUp(i)   { if (i > 0) { const c = chain.value; [c[i-1], c[i]] = [c[i], c[i-1]]; } }
    function moveDown(i) { const c = chain.value; if (i < c.length - 1) { [c[i+1], c[i]] = [c[i], c[i+1]]; } }

    const preview = computed(() => JSON.stringify(chain.value));

    let _s = false;
    function syncFrom() {
      _s = true;
      try {
        const v = JSON.parse(props.modelValue || '[]');
        if (Array.isArray(v))
          chain.value = v
            .filter(x => x && typeof x.app === 'string' && x.app.length > 0)
            .map(x => ({ app: x.app, data: x.data || '' }));
        else
          chain.value = [];
      } catch { chain.value = []; }
      _s = false;
    }
    function syncTo() {
      if (_s) return;
      const clean = chain.value
        .filter(x => x && x.app)
        .map(x => x.data ? { app: x.app, data: x.data } : { app: x.app });
      emit('update:modelValue', JSON.stringify(clean));
    }

    watch(() => props.modelValue, syncFrom);
    watch(chain, syncTo, { deep: true });
    syncFrom();

    return { chain, nextApp, appGroups, addApp, removeAt, moveUp, moveDown,
             appHint, needsData, dataPlaceholder, preview };
  },
};

// 7) HA 节点列表（node_list_json → JSON 数组）
const HaNodeListEditor = {
  props: ['modelValue'],
  template: `
    <div class="struct-editor">
      <div class="struct-header">
        <span class="font-semibold">集群节点（{{ rows.length }} 个）</span>
        <span class="text-gray-400 text-[11px]">node_id/host/port/role(master|slave|witness)</span>
      </div>
      <div v-if="!rows.length" class="px-3 py-6 text-center text-gray-400 text-sm">暂无节点</div>
      <div v-for="(row, idx) in rows" :key="idx" class="struct-row">
        <input v-model="row.node_id" placeholder="node-1" class="w-28">
        <input v-model="row.host" placeholder="10.0.0.25" class="w-36">
        <input v-model.number="row.port" type="number" min="1" max="65535" placeholder="8080" class="w-20">
        <select v-model="row.role" class="w-24">
          <option value="master">master</option>
          <option value="slave">slave</option>
          <option value="witness">witness</option>
        </select>
        <i class="fa-solid fa-trash struct-del ml-auto" @click="rows.splice(idx,1)"></i>
      </div>
      <div class="struct-add" @click="rows.push({node_id:'',host:'',port:8080,role:'slave'})"><i class="fa-solid fa-plus mr-1"></i>添加节点</div>
    </div>
  `,
  setup(props, { emit }) {
    const rows = ref([]);
    let _s = false;
    function syncFrom() {
      _s = true;
      const v = JSON.parse(props.modelValue || '[]');
      rows.value = Array.isArray(v) ? v : [];
      if (!rows.value.length) rows.value.push({node_id:'',host:'',port:8080,role:'slave'});
      _s = false;
    }
    function syncTo() { if (_s) return; emit('update:modelValue', JSON.stringify(rows.value)); }
    watch(() => props.modelValue, syncFrom);
    watch(rows, syncTo, { deep: true });
    syncFrom();
    return { rows };
  },
};

// ===== App =====
const app = createApp({
  setup() {
    const auth = reactive({
      token: localStorage.getItem('nsbc_token') || null,
      user: localStorage.getItem('nsbc_user') || '',
      role: localStorage.getItem('nsbc_role') || '',
      loading: false, error: null,
    });
    const loginForm = reactive({ username: 'admin', password: 'admin', captcha_code: '' });
    const captchaSvg = ref(null);
    const captchaId = ref(null);
    const clientIp = ref('---');
    const route = ref(location.hash.replace('#', '') || '/realtime');

    // ===== 多租户全局状态 =====
    const tenantId = ref(parseInt(localStorage.getItem('nsbc_tenant_id') || '1', 10) || 1);
    const tenantOptions = ref([]);  // [{value, label}]
    const tenantLoading = ref(false);
    function changeTenant(id) {
      tenantId.value = id;
      localStorage.setItem('nsbc_tenant_id', id.toString());
      // 同步写入 SIP realm（WebRTC 页面和独立 webrtc.html 都会读这个）
      const t = tenantOptions.value.find(x => x.value === id);
      if (t && t.label) {
        const tn = t.label.replace(/\s*\(禁用\)\s*$/, '').trim();
        try { localStorage.setItem('nsbc_sip_domain', tn); } catch(_) {}
      }
      // 广播给所有 crudTable 重新加载
      window.dispatchEvent(new CustomEvent('nsbc-tenant-changed', { detail: id }));
    }
    async function fetchTenants() {
      tenantLoading.value = true;
      try {
        const r = await API.get('/tenants?limit=500&offset=0');
        tenantOptions.value = (r.items || []).map(t => ({
          value: t.id,
          label: t.tenant_name + (t.enabled ? '' : ' (禁用)'),
        }));
      } catch (e) { tenantOptions.value = []; }
      finally { tenantLoading.value = false; }
    }
    // 暴露给外部（pages-crud.js crudTable 工厂需要）
    window.__tenantId = tenantId;

    const menuItems = [
      // 实时监控（置于顶部，运维高频）
      { path: '/realtime',      label: '实时总览', icon: 'fa-solid fa-gauge-high' },
      { path: '/channels',     label: '在途呼叫', icon: 'fa-solid fa-phone-volume' },
      { path: '/cdrs',          label: '通话记录', icon: 'fa-solid fa-file-lines' },
      { path: '/metrics',       label: '性能指标', icon: 'fa-solid fa-chart-line' },
      { path: '/registrations', label: '注册状态', icon: 'fa-solid fa-user-check' },
      { path: '/security-center', label: '安全中心', icon: 'fa-solid fa-shield-halved' },
      // 业务数据
      { path: '/tenants',      label: '租户（SIP Realm）', icon: 'fa-solid fa-building' },
      { path: '/extensions',   label: '分机',     icon: 'fa-solid fa-phone' },
      { path: '/sip-profiles', label: 'SIP Profile', icon: 'fa-solid fa-network-wired' },
      { path: '/gateways',     label: '网关',     icon: 'fa-solid fa-tower-broadcast' },
      { path: '/trunks',       label: '中继',     icon: 'fa-solid fa-signal' },
      // 路由组已移到后端核心侧（nsbcd route_group_t），Web 侧暂不暴露
      { path: '/dialplan-rules', label: '拨号规则', icon: 'fa-solid fa-code-branch' },
      { path: '/time-groups',  label: '时间策略', icon: 'fa-solid fa-clock' },
      { path: '/queues',       label: '分机队列', icon: 'fa-solid fa-people-group' },
      { path: '/ivr-menus',    label: 'IVR 菜单', icon: 'fa-solid fa-circle-question' },
      // 计费与媒体
      { path: '/rates',        label: '费率管理', icon: 'fa-solid fa-coins' },
      { path: '/balances',     label: '账户余额', icon: 'fa-solid fa-wallet' },
      { path: '/transcode',    label: '媒体会话', icon: 'fa-solid fa-microphone' },
      // 系统
      { path: '/ha-groups',    label: '高可用组', icon: 'fa-solid fa-server' },
      // 工具
      { path: '/webrtc',       label: 'WebRTC 测试', icon: 'fa-solid fa-video' },
    ];

    window.addEventListener('hashchange', () => { route.value = location.hash.replace('#', '') || '/extensions'; });

    async function refreshCaptcha() {
      try {
        const r = await API.get('/auth/captcha');
        captchaId.value = r.captcha_id;
        captchaSvg.value = r.svg;
      } catch (e) { /* 验证码加载失败不阻塞登录 */ }
    }

    async function login() {
      auth.loading = true; auth.error = null;
      try {
        const body = { username: loginForm.username, password: loginForm.password };
        if (captchaId.value && loginForm.captcha_code) {
          body.captcha_id = captchaId.value;
          body.captcha_code = loginForm.captcha_code;
        }
        const r = await API.post('/auth/login', body);
        auth.token = r.access_token;
        auth.user  = loginForm.username;
        auth.role  = r.role || 'admin';
        localStorage.setItem('nsbc_token', r.access_token);
        localStorage.setItem('nsbc_user',  auth.user);
        localStorage.setItem('nsbc_role',  auth.role);
        if (r.refresh_token) localStorage.setItem('nsbc_refresh_token', r.refresh_token);
        // 登录成功后拉取租户列表
        fetchTenants();
        location.hash = '#/realtime';
      } catch (e) {
        auth.error = e.message;
        loginForm.captcha_code = '';
        refreshCaptcha();
      }
      finally { auth.loading = false; }
    }
    function logout() { localStorage.clear(); auth.token = null; location.hash = '#/login'; }

    refreshCaptcha();
    window.__auth = auth;  // 暴露给 API 401 handler 使用

    // ===== 登录态自动恢复：页面刷新后也拉租户列表 =====
    // fetchTenants 原来只在 login() 成功回调里调 — 页面刷新（localStorage 有 token）
    // 不走 login()，导致 tenantOptions 永远为空 → 所有 tenant_id 下拉无选项
    if (auth.token) fetchTenants();

    return { auth, loginForm, captchaSvg, clientIp, route, menuItems, login, logout, refreshCaptcha,
             tenantId, tenantOptions, tenantLoading, changeTenant, fetchTenants };
  },
});

// 注册结构化编辑器
app.component('time-range-editor',    TimeRangeEditor);
app.component('queue-members-editor',  QueueMembersEditor);
app.component('ivr-options-editor',    IvrOptionsEditor);
app.component('trunk-codec-editor',    TrunkCodecEditor);
app.component('dp-action-editor',      DialplanActionEditor);
app.component('dp-app-chain-editor',    AppChainEditor);
app.component('ha-node-list-editor',   HaNodeListEditor);

// ===== Shared CRUD factory =====
// asyncLoaders: { [formKey]: async () => [{value, label}] }  动态下拉选项，onMounted 时并发拉取
function crudTable(entity, path, columns, formSchema, asyncLoaders = null) {
  return {
    template: `
      <div class="space-y-4">
        <div v-if="errorMsg" class="mb-2 p-3 bg-red-50 border border-red-200 rounded text-red-700 text-sm flex items-center gap-2">
          <i class="fa-solid fa-triangle-exclamation"></i>{{ errorMsg }}
        </div>
        <div class="flex items-center justify-between">
          <h2 class="text-lg font-semibold text-slate-800"><i class="${entity.icon} mr-2 text-blue-600"></i>${entity.label}</h2>
          <div class="flex items-center gap-2">
            <el-select v-if="hasBoolColumn" v-model="filterEnabled" placeholder="状态" size="small" style="width:110px" clearable>
              <el-option label="全部" value=""></el-option>
              <el-option label="启用" value="true"></el-option>
              <el-option label="禁用" value="false"></el-option>
            </el-select>
            <el-button type="primary" size="small" @click="openCreate">
              <el-icon><Plus /></el-icon>新建
            </el-button>
          </div>
        </div>

        <el-table :data="items" border stripe size="default" style="width:100%">
          <el-table-column type="index" label="#" width="55" align="center"></el-table-column>
          <el-table-column v-for="c in columns" :key="c.key" :prop="c.key" :label="c.label" min-width="120">
            <template #default="{ row }">
              <el-tag v-if="c.type === 'bool'" :type="row[c.key] ? 'success' : 'info'" effect="light" size="small" round>
                {{ row[c.key] ? '启用' : '禁用' }}
              </el-tag>
              <span v-else-if="c.type === 'json'" class="font-mono text-xs text-slate-500" :title="row[c.key]" style="max-width:200px;display:inline-block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">{{ row[c.key] }}</span>
              <span v-else class="text-slate-700">{{ row[c.key] != null && row[c.key] !== '' ? row[c.key] : '-' }}</span>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="180" align="center" fixed="right">
            <template #default="{ row }">
              <div style="display:flex; gap:4px; justify-content:center; align-items:center; flex-wrap:nowrap;">
                <el-button link type="primary" size="small" @click="openEdit(row)">
                  <el-icon><Edit /></el-icon>编辑
                </el-button>
                <el-popconfirm title="确认删除？" @confirm="del(row.id)">
                  <template #reference>
                    <el-button link type="danger" size="small">
                      <el-icon><Delete /></el-icon>删除
                    </el-button>
                  </template>
                </el-popconfirm>
              </div>
            </template>
          </el-table-column>
        </el-table>

        <div class="flex justify-end pt-2">
          <el-pagination
            v-model:current-page="page"
            :page-size="limit"
            :total="total"
            layout="total, prev, pager, next"
            small
            background
          ></el-pagination>
        </div>

        <el-dialog v-model="showModal" :title="(editingId ? '编辑' : '新建') + ' ${entity.label}'" width="720px" destroy-on-close>
          <el-form :model="form" label-position="top">
            <el-form-item v-for="f in formSchema" :key="f.key" :label="f.label">
              <template v-if="f.type === 'select'">
                <el-select v-model="form[f.key]" placeholder="请选择" style="width:100%" clearable>
                  <el-option v-for="o in f.options" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <template v-else-if="f.type === 'async-select'">
                <el-select v-model="form[f.key]" placeholder="加载中..." style="width:100%" clearable :loading="asyncLoading[f.key]">
                  <el-option v-for="o in asyncOptions[f.key]" :key="o.value" :label="o.label" :value="o.value"></el-option>
                </el-select>
              </template>
              <el-input-number v-else-if="f.type === 'number'" v-model="form[f.key]" :controls="false" style="width:100%"></el-input-number>
              <el-switch v-else-if="f.type === 'bool'" v-model="form[f.key]" active-text="启用" inactive-text="禁用"></el-switch>
              <template v-else-if="f.type === 'json'">
                <div class="w-full">
                  <div class="flex justify-between items-center mb-1">
                    <span class="text-xs text-slate-500">结构化数据（点击切换 Raw JSON）</span>
                    <el-button link size="small" type="primary" @click="rawMode[f.key] = !rawMode[f.key]">
                      {{ rawMode[f.key] ? '结构化编辑' : 'Raw JSON' }}
                    </el-button>
                  </div>
                  <el-input v-if="rawMode[f.key]" v-model="form[f.key]" type="textarea" :rows="6"
                    placeholder='有效 JSON，例如 {"clip":"44"}' resize="none"></el-input>
                  <trunk-codec-editor v-else-if="f.editor === 'trunk-codec'" v-model="form[f.key]"></trunk-codec-editor>
                  <dp-action-editor   v-else-if="f.editor === 'dp-action'"   v-model="form[f.key]"></dp-action-editor>
                  <time-range-editor  v-else-if="f.editor === 'time-ranges'"  v-model="form[f.key]"></time-range-editor>
                  <queue-members-editor v-else-if="f.editor === 'queue-members'" v-model="form[f.key]"></queue-members-editor>
                  <ivr-options-editor v-else-if="f.editor === 'ivr-options'"  v-model="form[f.key]"></ivr-options-editor>
                  <ha-node-list-editor v-else-if="f.editor === 'ha-node-list'"  v-model="form[f.key]"></ha-node-list-editor>
                  <el-input v-else v-model="form[f.key]" type="textarea" :rows="6" resize="none"
                    placeholder='输入 JSON 字符串'></el-input>
                </div>
              </template>
              <el-input v-else-if="f.type === 'textarea'" v-model="form[f.key]" type="textarea" :rows="3" resize="vertical"></el-input>
              <el-input v-else-if="f.type === 'password'" v-model="form[f.key]" type="password" show-password :placeholder="f.help || '请输入密码'"></el-input>
              <el-input v-else v-model="form[f.key]" :placeholder="'请输入 ' + f.label"></el-input>
              <div v-if="f.help" class="text-xs text-slate-400 mt-1">{{ f.help }}</div>
            </el-form-item>
          </el-form>
          <template #footer>
            <el-button @click="showModal = false">取消</el-button>
            <el-button type="primary" :loading="saving" @click="save">保 存</el-button>
          </template>
        </el-dialog>
      </div>
    `,
    props: [],
    setup() {
      const items = ref([]), total = ref(0), page = ref(1), limit = ref(50);
      const filterEnabled = ref('');
      const showModal = ref(false), editingId = ref(null);
      const form = reactive({}), saving = ref(false);
      const rawMode = reactive({});
      const errorMsg = ref('');  // 显示在列表上方的错误提示
      // async-select 动态选项
      const asyncOptions = reactive({});
      const asyncLoading = reactive({});

      const hasBoolColumn = columns.some(c => c.type === 'bool');
      const hasAsync = !!asyncLoaders;

      async function load() {
        const q = new URLSearchParams({ limit: limit.value, offset: (page.value - 1) * limit.value });
        if (filterEnabled.value) q.set('enabled', filterEnabled.value);
        errorMsg.value = '';
        try {
          const r = await API.get(path + '?' + q.toString());
          items.value = r.items || []; total.value = r.total || items.value.length;
        } catch (e) {
          items.value = []; total.value = 0;
          errorMsg.value = '加载失败: ' + (e.message || e) + '（检查后端是否已编译 / DB 是否可用 / 登录是否过期）';
        }
      }
      async function loadAsyncOptions() {
        if (!asyncLoaders) return;
        const tasks = Object.entries(asyncLoaders).map(async ([key, fn]) => {
          asyncLoading[key] = true;
          try { asyncOptions[key] = await fn(); }
          catch (e) { asyncOptions[key] = []; }
          finally { asyncLoading[key] = false; }
        });
        await Promise.all(tasks);
      }
      function openCreate() {
        Object.keys(form).forEach(k => delete form[k]);
        Object.keys(rawMode).forEach(k => delete rawMode[k]);
        formSchema.forEach(f => {
          form[f.key] = f.type === 'bool' ? true : (f.default ?? '');
          if (f.editor) rawMode[f.key] = false;
        });
        // 自动填入当前全局租户 ID（覆盖 default）
        if (formSchema.some(f => f.key === 'tenant_id')) {
          form.tenant_id = API.tenantId();
        }
        editingId.value = null; showModal.value = true;
      }
      function openEdit(row) {
        Object.keys(form).forEach(k => delete form[k]);
        Object.keys(rawMode).forEach(k => delete rawMode[k]);
        Object.assign(form, row);
        formSchema.forEach(f => { if (f.editor) rawMode[f.key] = false; });
        editingId.value = row.id; showModal.value = true;
      }
      function toggleRaw(k) { rawMode[k] = !rawMode[k]; }

      async function save() {
        saving.value = true;
        try {
          for (const f of formSchema) {
            if (f.type === 'json') {
              try { const v = form[f.key] || 'null'; if (v !== 'null') JSON.parse(v); }
              catch { alert(f.label + ' 不是有效 JSON'); saving.value = false; return; }
            }
          }
          const body = {};
          formSchema.forEach(f => {
            const v = form[f.key];
            if (v === undefined) return;
            // password 字段：空字符串也不传（编辑时留空 = 不改密码）
            if (f.type === 'password' && v === '') return;
            body[f.key] = v;
          });
          if (editingId.value) await API.put(path + '/' + editingId.value, body);
          else await API.post(path, body);
          showModal.value = false; await load();
        } catch (e) { alert('保存失败: ' + e.message); }
        finally { saving.value = false; }
      }
      async function del(id) {
        if (!confirm('确认删除 ID=' + id + '？')) return;
        try { await API.delete(path + '/' + id); await load(); }
        catch (e) { alert('删除失败: ' + e.message); }
      }

      onMounted(() => {
        load(); loadAsyncOptions();
        // 监听租户切换，自动重新加载当前实体列表
        const onTenantChanged = () => { load(); };
        window.addEventListener('nsbc-tenant-changed', onTenantChanged);
        onBeforeUnmount(() => {
          window.removeEventListener('nsbc-tenant-changed', onTenantChanged);
        });
      });
      watch([page, limit, filterEnabled], load);

      return { items, total, page, limit, filterEnabled, showModal, editingId, form, saving, rawMode,
               errorMsg, hasBoolColumn, hasAsync, columns, formSchema, asyncOptions, asyncLoading,
               openCreate, openEdit, toggleRaw, save, del };
    },
  };
}

// ===== 只读面板工厂 =====
function livePanel(title, icon, columns, apiPath, opts = {}) {
  const { refreshMs = 5000, actionCol = null, pageSize = 20, sortBy = null, sortDir = 'desc' } = opts;
  return {
    template: `
      <div>
        <div class="flex items-center justify-between mb-4">
          <h2 class="text-lg font-semibold"><i :class="icon"></i> ${title}</h2>
          <div class="flex items-center gap-2 text-xs text-gray-500">
            <span v-if="loading"><i class="fa-solid fa-spinner fa-spin"></i> 加载中</span>
            <span v-else-if="error" class="text-red-500">⚠ {{ error }}</span>
            <span v-else>✓ 已刷新 {{ lastUpdated }}</span>
            <button @click="load" class="px-2 py-1 border rounded hover:bg-gray-50">刷新</button>
          </div>
        </div>
        <div v-if="!allRows.length && !loading" class="text-center text-gray-400 py-12">
          暂无数据
        </div>
        <div v-else class="overflow-x-auto border rounded">
        <table class="w-full text-sm border-collapse min-w-[1400px]">
          <thead>
            <tr class="bg-gray-50 border-b">
              <th v-for="c in columns" :key="c.key" class="px-3 py-2 text-left font-medium text-gray-600 whitespace-nowrap">{{ c.label }}</th>
              <th v-if="actionCol" class="px-3 py-2 text-left font-medium text-gray-600 sticky right-0 bg-gray-50 border-l">操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="(row, idx) in pagedRows" :key="idx" class="border-b hover:bg-blue-50/50">
              <td v-for="c in columns" :key="c.key" class="px-3 py-2 font-mono text-xs text-gray-700 whitespace-nowrap">
                <template v-if="row[c.key] !== undefined && row[c.key] !== null && row[c.key] !== ''">
                  {{ c.fmt ? c.fmt(row[c.key]) : row[c.key] }}
                </template>
                <template v-else>-</template>
              </td>
              <td v-if="actionCol" class="px-3 py-2 sticky right-0 bg-white border-l">
                <span v-html="actionCol(row)"></span>
              </td>
            </tr>
          </tbody>
        </table>
        </div>
        <!-- 分页控件 -->
        <div v-if="allRows.length > pageSize" class="flex items-center justify-between mt-3 text-xs text-gray-500">
          <div>共 {{ allRows.length }} 条 · 每页 {{ pageSize }} 条</div>
          <div class="flex items-center gap-1">
            <button @click="currentPage = 1" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">«</button>
            <button @click="currentPage--" :disabled="currentPage===1" class="px-2 py-1 border rounded disabled:opacity-30">‹</button>
            <span class="px-2">{{ currentPage }} / {{ totalPages }}</span>
            <button @click="currentPage++" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">›</button>
            <button @click="currentPage = totalPages" :disabled="currentPage===totalPages" class="px-2 py-1 border rounded disabled:opacity-30">»</button>
          </div>
        </div>
      </div>
    `,
    setup() {
      const allRows = ref([]);
      const loading = ref(false);
      const error = ref(null);
      const lastUpdated = ref('');
      const currentPage = ref(1);
      let timer = null;

      const totalPages = computed(() => Math.max(1, Math.ceil(allRows.value.length / pageSize)));
      const pagedRows = computed(() => {
        const start = (currentPage.value - 1) * pageSize;
        return allRows.value.slice(start, start + pageSize);
      });

      async function load(silent) {
        if (silent !== true) loading.value = true;
        error.value = null;
        try {
          const data = await API.get(apiPath);
          let rows = extractRows(data);
          // 排序
          if (sortBy) {
            rows = rows.slice().sort((a, b) => {
              let va = a[sortBy], vb = b[sortBy];
              if (va == null) va = 0; if (vb == null) vb = 0;
              if (typeof va === 'string') return sortDir === 'asc' ? va.localeCompare(vb) : vb.localeCompare(va);
              return sortDir === 'asc' ? va - vb : vb - va;
            });
          }
          allRows.value = rows;
          if (currentPage.value > totalPages.value) currentPage.value = totalPages.value;
          lastUpdated.value = new Date().toLocaleTimeString();
        } catch (e) { error.value = e.message; }
        finally { if (silent !== true) loading.value = false; }
      }

      function extractRows(data) {
        if (!data) return [];
        return data.rates || data.accounts || data.sessions || data.calls || data.cdrs
            || data.items  // 后端分页结构
            || (data.metrics && Object.entries(data.metrics).map(([k,v]) => ({metric: k, value: v})))
            || (Array.isArray(data) ? data : []);
      }

      onMounted(() => { load(); if (refreshMs > 0) timer = setInterval(load, refreshMs); });
      // nsbc-esl-web §8#1：SSE 推送触发静默刷新（不闪 loading），与 refreshMs
      // 轮询构成 push+pull 双路径。修复：清定时器须在卸载时（原误用 onMounted，
      // 导致 setInterval 刚建即被清除，轮询实际不生效）。
      const onSseRefresh = () => load(true);
      onMounted(() => document.addEventListener('nsbc-refresh', onSseRefresh));
      onBeforeUnmount(() => { if (timer) clearInterval(timer); document.removeEventListener('nsbc-refresh', onSseRefresh); });
      return { allRows, loading, error, lastUpdated, load, columns, icon, currentPage, totalPages, pagedRows, actionCol };
    },
  };
}

// 所有 const / function 在经典 script 标签间自动共享全局词法作用域，
// 后续 pages-crud.js / pages-live.js 可直接使用 app, crudTable, livePanel, API 等。

// ===== 全局挂断函数（livePanel actionCol 用 v-html + onclick 调） =====
window.nsbcHangup = async function(callId) {
  if (!callId) return;
  if (!confirm('确认挂断通话 ' + callId + ' ？')) return;
  try {
    await API.post('/calls/' + encodeURIComponent(callId) + '/hangup', {});
    // 触发所有 livePanel 刷新
    document.dispatchEvent(new CustomEvent('nsbc-refresh'));
    alert('挂断指令已发送');
  } catch (e) {
    alert('挂断失败: ' + (e.message || e));
  }
};

// ===== SESSION 实时推送消费者（SSE，nsbc-esl-web §8#1）=====
// 双路径设计（符合项目「订阅推送 + 拉取」约束）：本消费者把服务端 /calls/stream
// 的 SESSION 事件转成本地 nsbc-refresh 事件即时刷新 livePanel；若 SSE 不可用，
// 各面板的 refreshMs 轮询自动兜底，无感降级。
// 说明：EventSource 无法携带 Authorization 头，故用 fetch + ReadableStream 手解析
// SSE 帧（event:/data: 行，空行分隔）；连接断开按指数退避（1s→30s）重连。
(function () {
  const MAX_BACKOFF = 30000;
  let refreshTimer = null;
  let backoff = 1000;

  // 合并突发事件：一次呼叫会有 create/answer/hold/... 多个事件，300ms 去抖后
  // 只触发一次面板刷新，避免高频重拉。
  function scheduleRefresh() {
    if (refreshTimer) return;
    refreshTimer = setTimeout(() => {
      refreshTimer = null;
      document.dispatchEvent(new CustomEvent('nsbc-refresh'));
    }, 300);
  }

  async function streamLoop() {
    const token = API.token();
    if (!token) { setTimeout(streamLoop, 2000); return; }  // 未登录：稍后重试
    let res;
    try {
      const h = API.headers(false);
      h['Accept'] = 'text/event-stream';
      res = await fetch(API.base + '/calls/stream', { headers: h });
      if (!res.ok || !res.body) throw new Error('HTTP ' + res.status);
    } catch (e) {
      console.warn('[live] SSE 连接失败，' + backoff + 'ms 后重连：', e.message || e);
      setTimeout(streamLoop, backoff);
      backoff = Math.min(backoff * 2, MAX_BACKOFF);
      return;
    }
    backoff = 1000;  // 连接成功，重置退避
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let idx;
        while ((idx = buf.indexOf('\n\n')) >= 0) {
          const frame = buf.slice(0, idx);
          buf = buf.slice(idx + 2);
          const m = /(^|\n)event:\s*([^\n]+)/.exec(frame);
          const ev = m ? m[2].trim() : 'message';
          if (ev === 'session') scheduleRefresh();
        }
      }
    } catch (e) {
      console.warn('[live] SSE 读取中断：', e.message || e);
    }
    setTimeout(streamLoop, backoff);        // 断线重连
    backoff = Math.min(backoff * 2, MAX_BACKOFF);
  }

  // 延迟启动，等首帧/登录态就绪
  setTimeout(streamLoop, 1500);
})();
