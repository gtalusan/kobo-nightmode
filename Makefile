debug: all
all:
	$(MAKE) $(MAKECMDGOALS) -C src
	$(MAKE) $(MAKECMDGOALS) -C installer
	$(MAKE) $(MAKECMDGOALS) -C updateLib
	$(MAKE) -C uninstaller
	rm -f kobo-nightmode_build*.zip
	zip -qr kobo-nightmode_build14.zip installer/KoboRoot.tgz uninstaller/KoboRoot.tgz extra
