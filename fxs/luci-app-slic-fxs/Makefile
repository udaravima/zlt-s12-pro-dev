# SPDX-License-Identifier: GPL-2.0-only
#
# LuCI pages for the RJ11 phone port: Services → Phone → Status and
# Phone line. Plain package.mk (not luci.mk), so it builds in an SDK without
# the luci feed; nothing is compiled. Doc 16.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-slic-fxs
PKG_VERSION:=2026.11.17
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-slic-fxs
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI pages for the RJ11 phone port (status, line settings)
  DEPENDS:=+luci-base +rpcd-mod-ucode +ucode-mod-fs +ucode-mod-ubus +ucode-mod-uci +slic-fxs
  PKGARCH:=all
endef

define Package/luci-app-slic-fxs/description
  Services → Phone in LuCI. "Status" shows the RJ11 line (daemon, line
  state, handset, last call, recent events) and, if Asterisk is installed,
  the PBX: SIP phones with where they registered from, active calls, HEP
  capture, recent problems from Asterisk's log, and a warning when a module
  could not hook into PJSIP's full module table. "Phone line" edits
  /etc/config/slic. Data comes from one read-only rpcd method, luci.slic status.
endef

define Build/Compile
endef

define Package/luci-app-slic-fxs/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/slic
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/slic/status.js \
		./htdocs/luci-static/resources/view/slic/line.js \
		$(1)/www/luci-static/resources/view/slic/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-slic-fxs.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-slic-fxs.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_DATA) ./root/usr/share/rpcd/ucode/luci.slic $(1)/usr/share/rpcd/ucode/
endef

# what luci.mk's scripts do: drop LuCI's menu cache, and reload rpcd so it
# reads the new ACL and plugin (SIGHUP: rpcd re-executes, keeping sessions)
define Package/luci-app-slic-fxs/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache*
	rm -rf /tmp/luci-modulecache/
	killall -HUP rpcd 2>/dev/null
}
exit 0
endef

define Package/luci-app-slic-fxs/postrm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache*
	rm -rf /tmp/luci-modulecache/
	killall -HUP rpcd 2>/dev/null
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-slic-fxs))
