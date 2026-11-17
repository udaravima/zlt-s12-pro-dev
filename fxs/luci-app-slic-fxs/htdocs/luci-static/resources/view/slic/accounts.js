// SPDX-License-Identifier: GPL-2.0-only
//
// Services → Phone → Accounts (luci-app-slic-fxs): the PBX's SIP accounts,
// extensions 200-299. Everything goes through slic-accounts (slic-fxs r7),
// the same code slic-asterisk-setup uses. Listing needs read access;
// showing, adding, resetting or removing (anything touching a password)
// needs write access (rpcd ACL).
//
// SECURITY: as in status.js, every child passed to E() is in an array, so a
// string is always text, never HTML (contact URIs come from the phones).

'use strict';
'require view';
'require dom';
'require rpc';
'require ui';

const callAccounts = rpc.declare({ object: 'luci.slic', method: 'accounts', expect: { '': {} } });
const callStatus = rpc.declare({ object: 'luci.slic', method: 'status', expect: { '': {} } });
const callPassword = rpc.declare({ object: 'luci.slic', method: 'account_password', params: [ 'ext' ], expect: { '': {} } });
const callAdd = rpc.declare({ object: 'luci.slic', method: 'account_add', params: [ 'ext' ], expect: { '': {} } });
const callReset = rpc.declare({ object: 'luci.slic', method: 'account_reset', params: [ 'ext' ], expect: { '': {} } });
const callRemove = rpc.declare({ object: 'luci.slic', method: 'account_remove', params: [ 'ext' ], expect: { '': {} } });

function swallow(e) { return { error: e.message || String(e) }; }

// "sip:201@192.168.1.38:41868;transport=TCP" → "192.168.1.38:41868 (TCP)"
function contactAddr(uri) {
	const m = String(uri).match(/^sips?:(?:[^@]*@)?(\[[^\]]+\]|[^:;]+)(?::(\d+))?(.*)$/);
	if (!m)
		return uri;
	const t = (m[3].match(/;transport=(\w+)/) || [])[1];
	return m[1] + (m[2] ? ':' + m[2] : '') + (t ? ' (' + t.toUpperCase() + ')' : '');
}

// [{protocol:'udp', bind:'192.168.1.1:5060'}, {protocol:'tcp', ...}]
// → "192.168.1.1, port 5060 (UDP or TCP)"
function serverText(transports) {
	const byBind = {};
	(transports || []).forEach(function(t) {
		(byBind[t.bind] = byBind[t.bind] || []).push(String(t.protocol).toUpperCase());
	});
	const parts = Object.keys(byBind).map(function(b) {
		const m = b.match(/^(.*):(\d+)$/);
		return m ? _('%s, port %s (%s)').format(m[1], m[2], byBind[b].join(_(' or ')))
		         : '%s (%s)'.format(b, byBind[b].join(_(' or ')));
	});
	return parts.length ? parts.join('; ') : _('unknown (no transport in pjsip.conf)');
}

function kvTable(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		]);
	}));
}

function failed(what, r) {
	ui.addNotification(null, E('p', {}, [ _('%s: %s').format(what, r.error || _('unknown error')) ]), 'danger');
}

return view.extend({
	load: function() {
		return Promise.all([ callAccounts().catch(swallow), callStatus().catch(swallow) ]);
	},

	refresh: function() {
		const self = this;
		return this.load().then(function(d) { dom.content(self.body, self.renderBody(d)); });
	},

	// the dialog after add/reset: everything a phone needs
	showCredentials: function(title, ext, password, notes) {
		ui.showModal(title, [
			E('p', {}, [ _('Enter these in the phone or softphone. "Show" on this page displays the password again later.') ]),
			kvTable([
				[ _('SIP server'), serverText(this.transports) ],
				[ _('Username'), ext ],
				[ _('Password'), E('code', {}, [ password || '?' ]) ]
			]),
			(notes || []).length ? E('p', {}, [ E('em', {}, [ notes.join(' ') ]) ]) : '',
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'cbi-button cbi-button-positive', 'click': ui.hideModal }, [ _('Done') ])
			])
		]);
	},

	confirm: function(title, text, label, style, action) {
		ui.showModal(title, [
			E('p', {}, [ text ]),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'cbi-button', 'click': ui.hideModal }, [ _('Cancel') ]),
				' ',
				E('button', { 'class': 'cbi-button ' + style, 'click': ui.createHandlerFn(this, action) }, [ label ])
			])
		]);
	},

	// the Show button of one row; Hide puts it back
	showButton: function(ext, cell) {
		return E('button', { 'class': 'cbi-button',
			'click': ui.createHandlerFn(this, 'handleShow', ext, cell) }, [ _('Show') ]);
	},

	handleShow: function(ext, cell) {
		const self = this;
		return callPassword(ext).catch(swallow).then(function(r) {
			if (r.error)
				return failed(_('Show password'), r);
			dom.content(cell, [
				E('code', {}, [ r.password ]),
				' ',
				E('button', { 'class': 'cbi-button', 'click': function() {
					dom.content(cell, [ self.showButton(ext, cell) ]);
				} }, [ _('Hide') ])
			]);
		});
	},

	// not "handleReset": LuCI views use that name for the form's Reset button
	handleNewPassword: function(ext) {
		this.confirm(_('New password for %s?').format(ext),
			_('The phone logged in as %s must then be given the new password: its next login with the old one is refused. Calls in progress carry on.').format(ext),
			_('New password'), 'cbi-button-action', function() {
				return callReset(ext).catch(swallow).then(function(r) {
					if (r.error)
						return ui.hideModal(), failed(_('New password'), r);
					this.showCredentials(_('New password for %s').format(ext), ext, r.password, r.notes);
					return this.refresh();
				}.bind(this));
			}.bind(this));
	},

	handleRemove: function(ext) {
		this.confirm(_('Remove extension %s?').format(ext),
			_('The phone logged in as %s is logged out, and calls to %s fail. Its password is deleted; adding %s again gives it a new one.').format(ext, ext, ext),
			_('Remove'), 'cbi-button-negative', function() {
				return callRemove(ext).catch(swallow).then(function(r) {
					ui.hideModal();
					if (r.error)
						return failed(_('Remove'), r);
					ui.addNotification(null, E('p', {}, [ _('Extension %s removed.').format(ext) ]), 'info');
					return this.refresh();
				}.bind(this));
			}.bind(this));
	},

	handleAdd: function(input, existing) {
		const ext = String(input.value || input.placeholder || '').trim();
		if (!/^2\d\d$/.test(ext))
			return failed(_('Add'), { error: _('use a number from 200 to 299') });
		if (existing.indexOf(ext) >= 0)
			return failed(_('Add'), { error: _('extension %s already exists').format(ext) });
		return callAdd(ext).catch(swallow).then(function(r) {
			if (r.error)
				return failed(_('Add'), r);
			this.showCredentials(_('Extension %s added').format(ext), ext, r.password, r.notes);
			return this.refresh();
		}.bind(this));
	},

	renderBody: function(d) {
		const acc = d[0] || {}, st = d[1] || {};
		if (acc.error)
			return E('div', { 'class': 'alert-message error' }, [ _('Could not read the accounts: %s').format(acc.error) ]);
		if (!acc.available)
			return E('div', { 'class': 'alert-message warning' },
				[ _('slic-accounts is not installed: this page needs slic-fxs r7 or later.') ]);

		this.transports = acc.transports;
		const exts = acc.accounts || [];
		const eps = {};
		((st.pbx || {}).endpoints || []).forEach(function(ep) { eps[ep.name] = ep; });

		let next = '';
		for (let n = 201; n <= 299 && !next; n++)
			if (exts.indexOf(String(n)) < 0)
				next = String(n);

		const rows = exts.map(function(ext) {
			const c = (eps[ext] || {}).contacts || [];
			const cell = E('td', { 'class': 'td left', 'data-title': _('Password') }, []);
			dom.content(cell, [ this.showButton(ext, cell) ]);
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'data-title': _('Extension') }, [ E('strong', {}, [ ext ]) ]),
				E('td', { 'class': 'td left', 'data-title': _('Logged in from') },
					c.length ? c.map(function(x, i) { return E('div', {}, [ contactAddr(x.uri) ]); })
					         : [ E('em', {}, [ _('not logged in') ]) ]),
				cell,
				E('td', { 'class': 'td right' }, [
					E('button', { 'class': 'cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, 'handleNewPassword', ext) }, [ _('New password') ]),
					' ',
					E('button', { 'class': 'cbi-button cbi-button-negative',
						'click': ui.createHandlerFn(this, 'handleRemove', ext) }, [ _('Remove') ])
				])
			]);
		}, this);

		const input = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'maxlength': '3',
		                           'size': '5', 'placeholder': next, 'aria-label': _('Extension') });

		return E([], [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Where phones connect') ]),
				kvTable([
					[ _('SIP server'), serverText(acc.transports) ],
					[ _('Username'), _('the extension number, e.g. %s').format(exts[0] || '201') ],
					[ _('Password'), _('per extension: "Show" below') ],
					[ _('Reachable from'), _('the LAN only: there is no port forward, and Asterisk accepts these accounts only from LAN addresses') ]
				])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Extensions') ]),
				E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr table-titles' }, [
						E('th', { 'class': 'th left' }, [ _('Extension') ]),
						E('th', { 'class': 'th left' }, [ _('Logged in from') ]),
						E('th', { 'class': 'th left' }, [ _('Password') ]),
						E('th', { 'class': 'th right' }, [ ' ' ])
					])
				].concat(rows.length ? rows : [
					E('tr', { 'class': 'tr placeholder' }, [
						E('td', { 'class': 'td', 'colspan': '4' }, [ E('em', {}, [ _('No SIP accounts yet') ]) ])
					])
				])),
				E('div', { 'class': 'cbi-section-descr' },
					[ _('Extension 100 is the RJ11 phone; it isn\'t a SIP account and isn\'t listed here.') ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Add an extension') ]),
				E('div', { 'class': 'cbi-section-descr' },
					[ _('A free number from 200 to 299. It gets a random 20-character password, and since the dialplan already rings every 2XX number, nothing else needs changing.') ]),
				E('div', {}, [
					input, ' ',
					E('button', { 'class': 'cbi-button cbi-button-add',
						'click': ui.createHandlerFn(this, 'handleAdd', input, exts) }, [ _('Add') ])
				])
			])
		]);
	},

	render: function(d) {
		this.body = E('div', {}, [ this.renderBody(d) ]);
		return E([], [
			E('h2', {}, [ _('Phone: SIP accounts') ]),
			E('div', { 'class': 'cbi-map-descr' },
				[ _('The logins for SIP phones and softphones. Changes take effect at once, without restarting the PBX.') ]),
			this.body
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
