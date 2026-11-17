// SPDX-License-Identifier: GPL-2.0-only
//
// Services → Phone → Status (luci-app-slic-fxs): the RJ11 phone line and the
// Asterisk PBX, read-only. Data: "ubus call luci.slic status", from
// /usr/share/rpcd/ucode/luci.slic. Refreshes every 10 s while open.

'use strict';
'require view';
'require dom';
'require poll';
'require rpc';

const REFRESH = 10;	// s; each refresh costs the router about 0.4 s of CPU

const callStatus = rpc.declare({
	object: 'luci.slic',
	method: 'status',
	expect: { '': {} }
});

const LINE_STATE = {
	idle: _('Idle'),
	dialtone: _('Dial tone'),
	dialing: _('Dialling'),
	ringing: _('Ringing'),
	talk: _('In a call'),
	busy: _('Busy tone')
};

const CALL_PHASE = {
	ringing: _('ringing'),
	answered: _('answered'),
	connecting: _('connecting to Asterisk'),
	bridged: _('connected'),
	ended: _('ended'),
	busy: _('busy')
};

const CID_FORMAT = {
	bell: _('Bell 202 FSK, after the first ring'),
	v23: _('ITU-T V.23 FSK, before the first ring'),
	dtmf: _('DTMF, before the first ring')
};

// Asterisk device states, as "pjsip show endpoints" prints them
const EP_STATE = {
	'Not in use': _('Idle'),
	'In use': _('In a call'),
	'Busy': _('Busy'),
	'Ringing': _('Ringing'),
	'Ring+Inuse': _('Ringing, and in a call'),
	'On Hold': _('On hold'),
	'Unavailable': _('Not registered'),
	'Invalid': _('Invalid')
};

// log lines that look alarming but aren't (doc 12 §7): labelled, not hidden
const HARMLESS = [
	[ /The canary is no more|Unable to set regular thread priority/, _('harmless: Asterisk lowered its own priority') ],
	[ /no local system nameservers configured/, _('harmless: Asterisk uses the system resolver instead') ],
	[ /No music on hold classes configured|Error opening directory - \S*\/moh\b/, _('no music-on-hold files installed; matters only if you put calls on hold') ]
];

const QUALIFY = {
	Avail: _('reachable'),
	Unavail: _('not answering'),
	NonQual: _('not checked'),
	Unknown: _('unknown')
};

function duration(s) {
	if (s == null || isNaN(s))
		return '-';
	const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
	if (d)
		return _('%dd %dh %dm').format(d, h, m);
	if (h)
		return _('%dh %dm').format(h, m);
	return _('%dm %ds').format(m, s % 60);
}

// "sip:201@192.168.1.38:41868;transport=TCP" → "192.168.1.38:41868 (TCP)".
// Asterisk cuts long URIs to its column width: show whatever still parses.
function contactAddr(uri) {
	const m = String(uri).match(/^sips?:(?:[^@]*@)?(\[[^\]]+\]|[^:;]+)(?::(\d+))?(.*)$/);
	if (!m)
		return uri;
	const t = (m[3].match(/;transport=(\w+)/) || [])[1];
	return m[1] + (m[2] ? ':' + m[2] : '') + (t ? ' (' + t.toUpperCase() + ')' : '');
}

// SECURITY: LuCI's E(tag, attrs, child) puts a *single string* child in as
// HTML (innerHTML); only strings inside an *array* become text nodes. Much
// of what this page shows comes from outside: SIP caller IDs, contact URIs,
// and log lines quoting the From header of unauthenticated SIP requests. So
// every child below is passed in an array, never as a bare string.

function section(title, descr, content) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, [ title ]),
		descr ? E('div', { 'class': 'cbi-section-descr' }, [ descr ]) : '',
		content
	]);
}

function kvTable(rows) {
	return E('table', { 'class': 'table' }, rows.filter(function(r) { return r; }).map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		]);
	}));
}

function gridTable(head, rows, empty) {
	return E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, head.map(function(h) {
			return E('th', { 'class': 'th left' }, [ h ]);
		}))
	].concat(rows.length ? rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, r.map(function(c, i) {
			return E('td', { 'class': 'td left', 'data-title': head[i] }, [ c ]);
		}));
	}) : [
		E('tr', { 'class': 'tr placeholder' }, [
			E('td', { 'class': 'td', 'colspan': head.length }, [ E('em', {}, [ empty ]) ])
		])
	]));
}

function lines(list) {
	return list.length > 1 ? E([], list.reduce(function(acc, x, i) {
		return i ? acc.concat([E('br'), x]) : [x];
	}, [])) : (list[0] || '-');
}

// ---- warnings ------------------------------------------------------------

function alerts(st) {
	const l = st.line || {}, p = st.pbx || {}, hep = p.hep || {}, a = [];

	if (!l.installed)
		a.push(['error', _('The slic-fxs package (the phone line daemon) is not installed.')]);
	else if (l.crash_guard)
		a.push(['error', _('At an earlier boot the phone line\'s kernel modules started loading and never finished, so they are not loaded now (the boot-loop guard /etc/slic.loading is still there). Read the system log first, then: rm /etc/slic.loading && /etc/init.d/slic start')]);
	else if (!l.enabled)
		a.push(['notice', _('The phone port is switched off (Phone line → Enabled).')]);
	else if (!l.running)
		a.push(['warning', _('The phone line daemon is not running: no dial tone, no calls. See "Recent line events" below and the system log.')]);

	if (l.running && l.applied) {
		const diff = [ 'cid_format', 'dial_timeout', 'audiosocket' ].filter(function(k) {
			return l.applied[k] != null && String(l.applied[k]) != String(l.config[k]);
		});
		if (diff.length)
			a.push(['notice', _('Saved settings differ from what the daemon is running with (%s). They apply when it restarts: /etc/init.d/slic restart').format(diff.join(', '))]);
	}

	if (p.installed && !p.running)
		a.push(['warning', _('Asterisk is not running: the handset gets busy tone on every number, and SIP phones can\'t register.')]);
	else if (p.running && !p.cli)
		a.push(['warning', _('Asterisk is running but did not answer on its control socket, so phones and calls can\'t be shown.')]);

	if (p.module_errors && p.module_errors.length)
		a.push(['error', E([], [
			_('Since Asterisk started, %d module(s) could not hook into its SIP stack. Asterisk lists them as Running, but they never see a SIP message. The usual cause: OpenWrt builds pjproject with room for 32 PJSIP modules, and they are all taken. Free a slot with a "noload" line for an unused res_pjsip module in /etc/asterisk/modules.conf, then restart Asterisk.').format(p.module_errors.length),
			E('pre', { 'style': 'white-space:pre-wrap;margin-top:.5em' }, [ p.module_errors.join('\n') ])
		])]);

	if (p.running && hep.enabled && !hep.sip_loaded)
		a.push(['warning', _('HEP capture is enabled in hep.conf, but res_hep_pjsip is not loaded, so nothing is captured.')]);

	return a.map(function(x) {
		return E('div', { 'class': 'alert-message ' + x[0] }, [ x[1] ]);
	});
}

// ---- sections ------------------------------------------------------------

function lineSection(l) {
	if (!l.installed)
		return '';

	const missing = Object.keys(l.modules || {}).filter(function(k) { return !l.modules[k]; });
	const cid = (l.running && l.applied.cid_format) || l.config.cid_format;
	const dt = (l.running && l.applied.dial_timeout) || l.config.dial_timeout;

	return section(_('Phone line (RJ11)'), null, E([], [
		kvTable([
			[ _('Daemon'), l.running ? _('Running (PID %d)').format(l.pid)
			                         : (l.enabled ? _('Not running') : _('Switched off')) ],
			l.running ? [ _('Line'), LINE_STATE[l.state] || l.state || '-' ] : null,
			l.running ? [ _('Handset'), l.hook == 'off' ? _('Lifted') : (l.hook == 'on' ? _('On hook') : '-') ] : null,
			l.call_phase ? [ _('Current or last call'), CALL_PHASE[l.call_phase] || l.call_phase ] : null,
			[ _('Caller ID format'), CID_FORMAT[cid] || cid ],
			[ _('Dial timeout'), _('%s s after the last key').format(dt) ],
			[ _('Kernel modules'), missing.length ? _('Missing: %s').format(missing.join(', ')) : _('Loaded') ]
		]),
		E('h4', {}, [ _('Recent line events') ]),
		gridTable([ _('Time'), _('Event') ],
			(l.events || []).map(function(e) { return [ e.time, e.text ]; }),
			_('Nothing in the system log yet'))
	]));
}

function pbxSection(p) {
	const hep = p.hep || {};
	let hepText = _('Not installed');

	if (hep.installed) {
		if (hep.enabled && hep.sip_loaded)
			hepText = _('On: every SIP message is copied to %s').format(hep.address || '?');
		else if (hep.enabled)
			hepText = _('Enabled in hep.conf, but not loaded');
		else
			hepText = _('Off');
	}

	return section(_('PBX (Asterisk)'), null, kvTable([
		[ _('Asterisk'), !p.installed ? _('Not installed')
			: (p.running ? _('Running, version %s (PID %d)').format(p.version || '?', p.pid) : _('Not running')) ],
		p.running ? [ _('Up for'), duration(p.uptime) ] : null,
		p.installed ? [ _('HEP capture'), hepText ] : null
	]));
}

function phonesSection(p) {
	if (!p.running || !p.cli)
		return '';

	return section(_('SIP phones'),
		_('One row per extension. "Registered from" is where the phone last logged in; reachability is Asterisk\'s own check, with the round trip in ms (phones that sleep answer slowly).'),
		gridTable([ _('Extension'), _('State'), _('Registered from'), _('Reachability'), _('Calls') ],
			(p.endpoints || []).map(function(ep) {
				const c = ep.contacts || [];
				return [
					ep.cid && ep.cid != ep.name ? '%s (%s)'.format(ep.name, ep.cid) : ep.name,
					c.length || ep.state != 'Unavailable' ? (EP_STATE[ep.state] || ep.state) : _('Not registered'),
					lines(c.map(function(x) { return contactAddr(x.uri); })),
					lines(c.map(function(x) {
						const rtt = parseFloat(x.rtt);
						return (QUALIFY[x.status] || x.status) + (rtt > 0 ? ', %d ms'.format(Math.round(rtt)) : '');
					})),
					String((ep.calls || []).length)
				];
			}),
			_('No SIP extensions configured')));
}

function callsSection(p) {
	if (!p.running || !p.cli)
		return '';

	return section(_('Active calls'),
		_('Every channel Asterisk has open; a call between two parties usually has two.'),
		gridTable([ _('Channel'), _('State'), _('Caller ID'), _('Number'), _('Application'), _('Duration') ],
			(p.channels || []).map(function(c) {
				return [
					c.channel, c.state, c.callerid || '-',
					c.exten ? '%s@%s'.format(c.exten, c.context) : '-',
					c.app ? (c.data ? '%s(%s)'.format(c.app, c.data) : c.app) : '-',
					duration(c.duration)
				];
			}),
			_('No calls')));
}

function problemsSection(p) {
	if (!p.installed || !p.log)
		return '';

	return section(_('Recent problems in Asterisk\'s log'),
		_('The last errors, warnings and refused SIP requests (a wrong password shows up here) from %s.').format(p.log),
		gridTable([ _('Time'), _('Level'), _('Message') ],
			(p.problems || []).slice().reverse().map(function(l) {
				// "[Nov 17 14:44:52] ERROR[12285] res_pjsip.c: Unable to ..."
				const m = l.match(/^\[([^\]]+)\]\s+(\w+)\[[^\]]*\]\s*(.*)$/);
				const h = HARMLESS.filter(function(x) { return x[0].test(l); })[0];
				const level = m ? m[2] : '-';
				return [ m ? m[1] : '-',
				         h ? E('span', { 'title': h[1] }, [ level + ' (' + h[1] + ')' ]) : level,
				         m ? m[3] : l ];
			}),
			_('Nothing recent')));
}

function renderAll(st) {
	if (st.error)
		return E('div', { 'class': 'alert-message error' },
			[ _('Could not read the status: %s').format(st.error) ]);

	const p = st.pbx || {};
	return E([], [].concat(alerts(st), [
		lineSection(st.line || {}),
		pbxSection(p),
		phonesSection(p),
		callsSection(p),
		problemsSection(p),
		E('div', { 'class': 'cbi-section-descr' }, [
			_('Updated %s; refreshes every %d seconds while this page is open.')
				.format(new Date(st.time * 1000).toLocaleTimeString(), REFRESH) ])
	]));
}

function load() {
	return callStatus().catch(function(e) { return { error: e.message || String(e) }; });
}

return view.extend({
	load: load,

	render: function(st) {
		const body = E('div', {}, renderAll(st));

		poll.add(function() {
			return load().then(function(s) { dom.content(body, renderAll(s)); });
		}, REFRESH);

		return E([], [
			E('h2', {}, [ _('Phone: status') ]),
			E('div', { 'class': 'cbi-map-descr' },
				[ _('The RJ11 phone line and the Asterisk PBX on this router. This page only reads; the line\'s settings are on the "Phone line" tab.') ]),
			body
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
