// SPDX-License-Identifier: GPL-2.0-only
//
// Offline test of the status page (doc 16 §6), no router or browser needed:
//
//   node fxs/luci-app-slic-fxs/test/render_test.js [status.js] [status.json]
//
// Runs view/slic/status.js against a strict stand-in for LuCI's E(). LuCI
// inserts a *single string* child as HTML (innerHTML), so the stand-in
// throws on one: that would be stored XSS, since caller IDs, contact URIs
// and Asterisk log lines come from outside. The data is a captured
// "ubus call luci.slic status" (status-sample.json, addresses anonymised)
// with attack strings injected into every outside-controlled field. Prints
// the page as text; exits non-zero on any failure.

'use strict';
const fs = require('fs');
const path = require('path');

const jsPath = process.argv[2] ||
	path.join(__dirname, '../htdocs/luci-static/resources/view/slic/status.js');
const dataPath = process.argv[3] || path.join(__dirname, 'status-sample.json');

// the parts of LuCI the page uses
String.prototype.format = function(...a) {
	let i = 0;
	return this.replace(/%[sdh]/g, () => String(a[i++]));
};
const isNode = x => x && typeof x === 'object' && x.__node;
function E(tag, attrs, children) {
	if (!(attrs instanceof Object) || Array.isArray(attrs) || isNode(attrs)) {
		children = attrs;
		attrs = null;
	}
	if (typeof children === 'string' || typeof children === 'number')
		throw new Error(`bare string child of <${Array.isArray(tag) ? 'fragment' : tag}> ` +
		                `(LuCI would parse it as HTML): ${String(children).slice(0, 60)}`);
	if (children != null && !Array.isArray(children) && !isNode(children))
		throw new Error(`unexpected child type ${typeof children} for <${tag}>`);
	const kids = children == null ? [] : (Array.isArray(children) ? children : [ children ]);
	return { __node: true, tag: Array.isArray(tag) ? '#frag' : tag, attrs: attrs || {}, kids };
}
const stub = {
	view: { extend: o => o },
	dom: { content() {} },
	poll: { add() {} },
	rpc: { declare: () => () => Promise.resolve({}) }
};

const src = fs.readFileSync(jsPath, 'utf8').replace(/^'require [^']+';$/mg, '');
const view = new Function('view', 'dom', 'poll', 'rpc', 'E', '_', src)(
	stub.view, stub.dom, stub.poll, stub.rpc, E, s => s);

const st = JSON.parse(fs.readFileSync(dataPath, 'utf8'));
const EVIL = '<img src=x onerror=alert(1)>';
st.line.events.push({ time: 'Nov 17 16:00:00', level: 'info', text: `caller ID bell '${EVIL}' sent` });
st.pbx.problems.push(`[Nov 17 16:00:00] NOTICE[1] res_pjsip/pjsip_distributor.c: Request 'REGISTER' ` +
                     `from '"${EVIL}" <sip:x@192.0.2.1>' failed for '192.0.2.1:5060'`);
st.pbx.module_errors = [ `[Nov 17 16:00:00] ERROR[1] res_pjsip.c: Unable to register module ${EVIL}` ];
st.pbx.endpoints[0].cid = EVIL;
st.pbx.endpoints[0].contacts.push({ uri: `sip:${EVIL}@<b>x</b>:5060`, status: 'Avail', rtt: '12.3' });
st.pbx.channels.push({ channel: `PJSIP/${EVIL}`, context: 'house', exten: '100', state: 'Up',
                       app: 'Dial', data: EVIL, callerid: EVIL, duration: 75, bridge: '' });

let tree;
try {
	tree = view.render(st);
} catch (e) {
	console.error('FAIL:', e.message);
	process.exit(1);
}

const out = [];
let evil = 0;
(function walk(n) {
	if (!isNode(n)) {
		const s = String(n);
		if (s.includes(EVIL))
			evil++;
		if (s.trim())
			out.push(s);
		return;
	}
	if (/^h[234]$/.test(n.tag)) out.push('\n## ');
	if (n.tag === 'tr') out.push('\n  ');
	if (n.tag === 'td' || n.tag === 'th') out.push(' | ');
	if (n.tag === 'div' && /alert-message/.test(n.attrs.class || ''))
		out.push(`\n[${n.attrs.class.replace('alert-message ', '').toUpperCase()}] `);
	n.kids.forEach(walk);
})(tree);

console.log(out.join('').replace(/\n\s*\n/g, '\n'));
if (evil < 7) {
	console.error(`FAIL: only ${evil} of 7 injected strings reached the page`);
	process.exit(1);
}
console.log(`\nOK: no bare string children; ${evil} injected strings rendered, all as text nodes`);
