include $(TOPDIR)/rules.mk

PKG_NAME:=slic-fxs
PKG_VERSION:=2026.10.08
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

# fxs/ checkout this package lives in (the SDK sees it through a symlink)
FXS_DIR:=$(shell readlink -f $(CURDIR))/..

include $(INCLUDE_DIR)/package.mk

define Package/slic-fxs
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=RJ11 phone port (Le9641 SLIC) daemon for the ZLT S12 Pro
  DEPENDS:=@TARGET_ramips_mt7621 +kmod-spi-dev +kmod-slic-spi-cs1 +kmod-mt7621-pcm-raw
endef

define Package/slic-fxs/description
  slicctl (Microsemi VP-API-II with the board's stock profiles), an init
  script that loads the SLIC modules late in boot with a boot-loop failsafe,
  and slic-play for playing raw 8 kHz clips to the handset.
endef

define Package/slic-fxs/conffiles
/etc/config/slic
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	[ -d $(FXS_DIR)/slicctl/vpapi ] || { echo "run fxs/slicctl/fetch-vpapi.sh first"; exit 1; }
	[ -f $(FXS_DIR)/slicctl/stock_profiles.c ] || { echo "generate fxs/slicctl/stock_profiles.c"; exit 1; }
	$(CP) $(FXS_DIR)/slicctl/. $(PKG_BUILD_DIR)/
	rm -rf $(PKG_BUILD_DIR)/vendor $(PKG_BUILD_DIR)/slicctl
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
	python3 $(FXS_DIR)/tools/gen_test_audio.py $(PKG_BUILD_DIR)/audio
endef

define Package/slic-fxs/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config $(1)/usr/share/slic
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/slicctl $(1)/usr/sbin/slicctl
	$(INSTALL_BIN) $(FXS_DIR)/tools/slic-play.sh $(1)/usr/sbin/slic-play
	$(INSTALL_BIN) ./files/slic.init $(1)/etc/init.d/slic
	$(INSTALL_CONF) ./files/slic.config $(1)/etc/config/slic
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/audio/*.s16 $(1)/usr/share/slic/
endef

$(eval $(call BuildPackage,slic-fxs))
