include ../Makefile.config

USER_PROGRAMS = restart.exe shutdown.exe login.exe 
# Add more programs here as your ecosystem grows
# USER_PROGRAMS += shell.exe editor.exe

all: $(USER_PROGRAMS)

%.exe: %.c ../library/user-start.o ../library/baselib.a
	@echo "------------------------------------------------"
	@echo "Configuring build for: $<"
	@# Prompting user and capturing response
	@res=$$(read -p "  Build as Flat Binary? (y/n): " choice && echo $$choice); \
	if [ "$$res" = "y" ] || [ "$$res" = "Y" ]; then \
		echo "  [FORMAT] Linking as FLAT BINARY (Stripped)..."; \
		$(CC) $(KERNEL_CCFLAGS) -I ../include -c $< -o $*.o; \
		$(LD) $(KERNEL_LDFLAGS) -Ttext 0x80000000 --oformat binary \
			../library/user-start.o $*.o ../library/baselib.a -o $@; \
		echo "  [DONE] Saved as $@ (Flat)"; \
	else \
		echo "  [FORMAT] Linking as ELF (Structured)..."; \
		$(CC) $(KERNEL_CCFLAGS) -I ../include -c $< -o $*.o; \
		$(LD) $(KERNEL_LDFLAGS) -Ttext 0x80000000 \
			../library/user-start.o $*.o ../library/baselib.a -o $@; \
		echo "  [DONE] Saved as $@ (ELF)"; \
	fi

clean:
	rm -rf *.exe *.o