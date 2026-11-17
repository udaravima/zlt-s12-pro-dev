// SPDX-License-Identifier: GPL-2.0-only
//
// Offline test of the LuCI pages (doc 16 §6), no router or browser needed:
//
//   node fxs/luci-app-slic-fxs/test/render_test.js
//
// Runs view/slic/status.js and accounts.js against strict stand-ins for
// LuCI's E(), dom.content() and ui.createHandlerFn():
//   - a *single string* child throws: LuCI would insert it as HTML, which is
//     stored XSS here (caller IDs, contact URIs and log lines come from
//     outside). Attack strings are injected into every such field.
//   - a button whose handler doesn't exist throws: LuCI would silently give
//     it no click handler (a method shadowed by a same-named key, say).
// The Accounts page's buttons are then clicked: Show, New password and
// Remove (each through its confirm dialog) and Add, checking the backend
// calls and what the page shows. status-sample.json is a captured
// "ubus call luci.slic status" with its addresses anonymised.
// Prints the status page as text; exits non-zero on any failure.

'use strict';
const fs = require('fs');
const path = require('path');

const VIEWS = path.join(__dirname, '../htdocs/luci-static/resources/view/slic');
const EVIL = '<img src=x onerror=alert(1)>';
let failures = 0;
const fail = (msg) => { console.error('FAIL:', msg); failures++; };

// ---- stand-ins for the parts of LuCI the pages use ----------------------

String.prototype.format = function(...a) {
	let i = 0;
	return this.replace(/%[sdh]/g, () => String(a[i++]));
};
const isNode = (x) => x && typeof x === 'object' && x.__node;

function kidsOf(where, children) {
	if (typeof children === 'string' || typeof children === 'number')
		throw new Error(`bare string child of <${where}> (LuCI would parse it as HTML): ` +
		                String(children).slice(0, 60));
	if (children != null && !Array.isArray(children) && !isNode(children))
		throw new Error(`unexpected child type ${typeof children} for <${where}>`);
	return children == null ? [] : (Array.isArray(children) ? children.slice() : [ children ]);
}

function E(tag, attrs, children) {
	if (!(attrs instanceof Object) || Array.isArray(attrs) || isNode(attrs)) {
		children = attrs;
		attrs = null;
	}
	const name = Array.isArray(tag) ? '#frag' : tag;
	const n = { __node: true, tag: name, attrs: attrs || {}, kids: kidsOf(name, children) };
	for (const k in n.attrs)		// DOM properties reflect attributes (value, placeholder)
		if (typeof n.attrs[k] === 'string')
			n[k] = n.attrs[k];
	return n;
}

const dom = {
	content(node, children) {
		node.kids = kidsOf(node.tag, children);
		return node;
	}
};

const modals = [], notes = [];
const ui = {
	createHandlerFn(ctx, fn, ...args) {
		const f = typeof fn === 'string' ? ctx[fn] : fn;
		if (typeof f !== 'function')
			throw new Error(`button handler ${typeof fn === 'string' ? `"${fn}"` : '?'} is not a function (LuCI would leave the button dead)`);
		return () => f.apply(ctx, args);
	},
	showModal(title, children) { modals.push({ title, kids: kidsOf('modal', children) }); },
	hideModal() {},
	addNotification(title, children, cls) { notes.push({ cls, kids: kidsOf('notification', children) }); }
};

function loadView(file, rpcImpl) {
	const rpc = { declare: (d) => (...a) => Promise.resolve(rpcImpl(d.method, a)) };
	const stubs = { view: { extend: (o) => o }, dom, poll: { add() {} }, rpc, ui };
	const src = fs.readFileSync(path.join(VIEWS, file), 'utf8').replace(/^'require [^']+';$/mg, '');
	return new Function('view', 'dom', 'poll', 'rpc', 'ui', 'E', '_', src)(
		stubs.view, stubs.dom, stubs.poll, stubs.rpc, stubs.ui, E, (s) => s);
}

function text(n) {
	return isNode(n) ? n.kids.map(text).join('') : String(n);
}
function find(n, pred, out = []) {
	if (!isNode(n))
		return out;
	if (pred(n))
		out.push(n);
	n.kids.forEach((k) => find(k, pred, out));
	return out;
}
const button = (root, label) => find(root, (n) => n.tag === 'button' && text(n) === label);

// ---- data: a captured status with attack strings in outside fields --------

const status = JSON.parse(fs.readFileSync(path.join(__dirname, 'status-sample.json'), 'utf8'));
status.line.events.push({ time: 'Nov 17 16:00:00', level: 'info', text: `caller ID bell '${EVIL}' sent` });
status.pbx.problems.push(`[Nov 17 16:00:00] NOTICE[1] res_pjsip/pjsip_distributor.c: Request 'REGISTER' ` +
                         `from '"${EVIL}" <sip:x@192.0.2.1>' failed for '192.0.2.1:5060'`);
status.pbx.module_errors = [ `[Nov 17 16:00:00] ERROR[1] res_pjsip.c: Unable to register module ${EVIL}` ];
status.pbx.endpoints[0].cid = EVIL;
status.pbx.endpoints[0].contacts.push({ uri: `sip:${EVIL}@<b>x</b>:5060`, status: 'Avail', rtt: '12.3' });
status.pbx.channels.push({ channel: `PJSIP/${EVIL}`, context: 'house', exten: '100', state: 'Up',
                           app: 'Dial', data: EVIL, callerid: EVIL, duration: 75, bridge: '' });

// ---- 1. status page ------------------------------------------------------

(function() {
	let tree;
	try {
		tree = loadView('status.js', () => status).render(status);
	} catch (e) {
		return fail(`status.js: ${e.message}`);
	}
	const out = [];
	let evil = 0;
	(function walk(n) {
		if (!isNode(n)) {
			const s = String(n);
			if (s.includes(EVIL)) evil++;
			if (s.trim()) out.push(s);
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
	if (evil < 7)
		fail(`status.js: only ${evil} of 7 injected strings reached the page`);
	else
		console.log(`\nstatus.js OK: ${evil} injected strings rendered, all as text`);
})();

// ---- 2. accounts page: render, then click every button -------------------

(async function() {
	const PW = 'TestTestTestTest0001';
	const accounts = { available: true, accounts: [ '201', '202', '203', '204', '205' ],
	                   transports: [ { protocol: 'udp', bind: '192.168.1.254:5060' },
	                                 { protocol: 'tcp', bind: '192.168.1.254:5060' } ] };
	const calls = [];
	const backend = (method, a) => {
		if (method === 'accounts') return accounts;
		if (method === 'status') return status;
		calls.push(`${method} ${a[0]}`);
		return method === 'account_remove' ? { ext: a[0], notes: [] } : { ext: a[0], password: PW, notes: [] };
	};
	try {
		const v = loadView('accounts.js', backend);
		const root = v.render(await v.load());
		// the page shows a contact's host:port (not its user part), so the
		// hostile host "<b>x</b>" must arrive, as text (kidsOf vets every child)
		if (!text(root).includes('<b>x</b>:5060'))
			fail('accounts.js: the hostile contact host did not reach the page');
		if (!text(root).includes('192.168.1.254, port 5060 (UDP or TCP)'))
			fail('accounts.js: server line wrong: ' + text(find(root, (n) => n.tag === 'table')[0]).slice(0, 120));

		// Show: the password appears in that row, Hide puts the button back
		await button(root, 'Show')[0].attrs.click();
		if (!text(root).includes(PW)) fail('accounts.js: Show did not display the password');
		button(root, 'Hide')[0].attrs.click();
		if (text(root).includes(PW)) fail('accounts.js: Hide left the password on the page');

		// New password: confirm dialog, then the credentials dialog
		modals.length = 0;
		button(root, 'New password')[0].attrs.click();
		await button({ __node: true, kids: modals[0].kids }, 'New password')[0].attrs.click();
		if (!modals.some((m) => /New password for 201/.test(m.title) && text({ __node: true, kids: m.kids }).includes(PW)))
			fail('accounts.js: no dialog with the new password after reset');

		// Remove: confirm dialog, then a notice
		modals.length = 0;
		button(v.body, 'Remove')[0].attrs.click();
		await button({ __node: true, kids: modals[0].kids }, 'Remove')[0].attrs.click();

		// Add: refused when invalid, then the suggested free number
		const input = find(v.body, (n) => n.tag === 'input')[0];
		input.value = '300';
		await button(v.body, 'Add')[0].attrs.click();
		input.value = '';
		await button(v.body, 'Add')[0].attrs.click();		// placeholder: next free = 206

		const want = [ 'account_password 201', 'account_reset 201', 'account_remove 201', 'account_add 206' ];
		if (JSON.stringify(calls) !== JSON.stringify(want))
			fail(`accounts.js: backend calls ${JSON.stringify(calls)}, expected ${JSON.stringify(want)}`);
		if (!notes.some((n) => n.cls === 'danger' && /200 to 299/.test(text({ __node: true, kids: n.kids }))))
			fail('accounts.js: "300" was not refused');
		if (!failures)
			console.log(`accounts.js OK: rendered (hostile contact URI as text), and Show/Hide, New password, Remove and Add made exactly: ${calls.join(', ')}`);
	} catch (e) {
		fail(`accounts.js: ${e.message}`);
	}
	process.exit(failures ? 1 : 0);
})();
