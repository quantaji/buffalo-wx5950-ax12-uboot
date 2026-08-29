obj-y   := ipq807x.o
obj-y	+= clock.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12_boot.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12_nandboot.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12_usbboot.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12_ui.o
obj-$(CONFIG_BUFFALO_WXR5950AX12) += wxr5950ax12_tftp.o

ifeq ($(CONFIG_WXR5950AX12_WEB),y)
obj-y += wxr5950ax12_web.o
obj-y += wxr5950ax12_web_data.o

quiet_cmd_wxr_web_data = WEB     $@
cmd_wxr_web_data = ( \
	echo '.section .rodata.wxr_web,"a"'; \
	echo '.balign 4'; \
	echo '.global wxr_web_page_begin'; \
	echo 'wxr_web_page_begin:'; \
	echo '.incbin "$<"'; \
	echo '.global wxr_web_page_end'; \
	echo 'wxr_web_page_end:'; \
	echo '.balign 4'; \
	) > $@

$(obj)/wxr5950ax12_web.html.gz: $(src)/wxr5950ax12_web.html FORCE
	$(call if_changed,gzip)

$(obj)/wxr5950ax12_web_data.S: $(obj)/wxr5950ax12_web.html.gz FORCE
	$(call if_changed,wxr_web_data)

targets += wxr5950ax12_web.html.gz wxr5950ax12_web_data.S
clean-files += wxr5950ax12_web.html.gz wxr5950ax12_web_data.S
endif
