// SPDX-License-Identifier: GPL-2.0-only
//
// Services → Phone → Phone line (luci-app-slic-fxs): a form over
// /etc/config/slic. Save & Apply commits it; the init script's reload
// trigger then restarts the daemon with the new environment.

'use strict';
'require view';
'require form';

return view.extend({
	render: function() {
		const m = new form.Map('slic', _('Phone: line settings'),
			_('Settings of the RJ11 phone port daemon (package slic-fxs, /etc/config/slic). Save & Apply restarts the daemon, which cuts off a call in progress.'));

		const s = m.section(form.NamedSection, 'main', 'slic');
		s.tab('general', _('General'));
		s.tab('advanced', _('Advanced'),
			_('Change these only with a reason: wrong values silence the line or stop calls. The defaults are the tested ones.'));

		let o = s.taboption('general', form.Flag, 'enabled', _('Enabled'),
			_('Off: no dial tone and no calls; the daemon stops at once. The kernel modules stay loaded until the next reboot, after which they aren\'t loaded at all.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.ListValue, 'cid_format', _('Caller ID format'),
			_('How the caller\'s number reaches the handset\'s display. Pick one your phone decodes; Bell 202 is the format confirmed on this project\'s test phone.'));
		o.value('bell', _('Bell 202 FSK, after the first ring'));
		o.value('v23', _('ITU-T V.23 FSK, before the first ring'));
		o.value('dtmf', _('DTMF, before the first ring'));
		o.default = 'bell';

		o = s.taboption('general', form.Value, 'dial_timeout', _('Dial timeout'),
			_('Seconds after the last key press before the number is dialled; # dials at once. Raise it if you dial slowly and get cut off. Default: 3.'));
		o.datatype = 'and(ufloat,range(1,10))';
		o.placeholder = '3';

		o = s.taboption('advanced', form.Value, 'audiosocket', _('AudioSocket address'),
			_('Where the daemon waits for Asterisk\'s audio connections. Asterisk\'s dialplan connects to the same address, so change both together. Only 127.0.0.1 is accepted: any other address would let the network reach the phone\'s audio. "off": no calls. Default: 127.0.0.1:4573.'));
		o.placeholder = '127.0.0.1:4573';
		o.validate = function(section_id, value) {
			if (value == null || value == '' || value == 'off')
				return true;
			const p = String(value).match(/^127\.0\.0\.1:(\d{1,5})$/);
			if (p && +p[1] >= 1 && +p[1] <= 65535)
				return true;
			return _('Use 127.0.0.1:PORT, or off');
		};

		o = s.taboption('advanced', form.ListValue, 'clk_switch', _('Audio clock'),
			_('Which chip clocks the phone\'s audio bus (GPIO 32). Takes effect at the next reboot.'));
		o.value('0', _('Router (needed for calls, caller ID and audio)'));
		o.value('1', _('Modem (stock VoLTE wiring: ringing works, but no calls or caller ID through this router)'));
		o.default = '0';

		o = s.taboption('advanced', form.Value, 'spi_hz', _('SPI clock (Hz)'),
			_('Clock of the control bus to the SLIC chip. 1000000 is tested; the stock firmware used up to 2048000. Takes effect at the next reboot.'));
		o.datatype = 'range(100000,2048000)';
		o.placeholder = '1000000';

		return m.render();
	}
});
