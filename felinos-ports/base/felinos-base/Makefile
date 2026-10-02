# FelinOS Base Filesystem Layout
# ==============================
# Cria estrutura básica de diretórios, /init, inittab, fstab, etc.

include ../../config.mk

PORT_DIR        := $(CURDIR)

# Necessário para os heredocs (`cat > arquivo <<- 'EOF' ... EOF`) abaixo: sem
# isso, o make roda cada LINHA da receita como uma chamada de shell separada,
# então o heredoc nunca encontraria seu próprio delimitador de fechamento (ele
# ficaria "em outra invocação do shell"). Com .ONESHELL, a receita inteira vai
# para um único `sh -c`, como um script de verdade.
.ONESHELL:

.PHONY: all install clean distclean

all: install

install:
	@echo "  INSTALL   felinos-base filesystem layout"
	# Diretórios base
	@mkdir -p $(SYSROOT)/usr/bin $(SYSROOT)/usr/sbin $(SYSROOT)/usr/lib \
	          $(SYSROOT)/usr/include $(SYSROOT)/usr/share $(SYSROOT)/usr/local \
	          $(SYSROOT)/etc/init.d $(SYSROOT)/etc/rc.d $(SYSROOT)/etc/profile.d \
	          $(SYSROOT)/dev $(SYSROOT)/proc $(SYSROOT)/sys $(SYSROOT)/run \
	          $(SYSROOT)/tmp $(SYSROOT)/var/log $(SYSROOT)/var/run \
	          $(SYSROOT)/var/spool $(SYSROOT)/var/lib $(SYSROOT)/var/lock \
	          $(SYSROOT)/var/mail $(SYSROOT)/var/opt $(SYSROOT)/root \
	          $(SYSROOT)/home $(SYSROOT)/boot $(SYSROOT)/mnt $(SYSROOT)/media \
	          $(SYSROOT)/srv
	
	# Symlinks padrão
	@ln -sf lib $(SYSROOT)/lib64 2>/dev/null || true
	@ln -sf usr/bin $(SYSROOT)/bin 2>/dev/null || true
	@ln -sf usr/sbin $(SYSROOT)/sbin 2>/dev/null || true
	@ln -sf usr/lib $(SYSROOT)/lib 2>/dev/null || true
	@ln -sf ../run $(SYSROOT)/var/run 2>/dev/null || true
	@ln -sf ../run/lock $(SYSROOT)/var/lock 2>/dev/null || true
	
	# /etc/passwd
	@cat > $(SYSROOT)/etc/passwd <<- 'EOF'
	root:x:0:0:root:/root:/bin/sh
	daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin
	bin:x:2:2:bin:/bin:/usr/sbin/nologin
	sys:x:3:3:sys:/dev:/usr/sbin/nologin
	sync:x:4:65534:sync:/bin:/bin/sync
	games:x:5:60:games:/usr/games:/usr/sbin/nologin
	man:x:6:12:man:/var/cache/man:/usr/sbin/nologin
	lp:x:7:7:lp:/var/spool/lpd:/usr/sbin/nologin
	mail:x:8:8:mail:/var/mail:/usr/sbin/nologin
	news:x:9:9:news:/var/spool/news:/usr/sbin/nologin
	uucp:x:10:10:uucp:/var/spool/uucp:/usr/sbin/nologin
	proxy:x:13:13:proxy:/bin:/usr/sbin/nologin
	www-data:x:33:33:www-data:/var/www:/usr/sbin/nologin
	backup:x:34:34:backup:/var/backups:/usr/sbin/nologin
	list:x:38:38:Mailing List Manager:/var/list:/usr/sbin/nologin
	irc:x:39:39:ircd:/var/run/ircd:/usr/sbin/nologin
	gnats:x:41:41:Gnats Bug-Reporting System (admin):/var/lib/gnats:/usr/sbin/nologin
	nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin
	felinos:x:1000:1000:FelinOS User:/home/felinos:/bin/sh
	EOF

	# /etc/group
	@cat > $(SYSROOT)/etc/group <<- 'EOF'
	root:x:0:
	daemon:x:1:
	bin:x:2:
	sys:x:3:
	adm:x:4:
	tty:x:5:
	disk:x:6:
	lp:x:7:
	mail:x:8:
	news:x:9:
	uucp:x:10:
	man:x:12:
	proxy:x:13:
	kmem:x:15:
	dialout:x:20:
	fax:x:21:
	voice:x:22:
	cdrom:x:24:
	floppy:x:25:
	tape:x:26:
	sudo:x:27:
	audio:x:29:
	dip:x:30:
	www-data:x:33:
	backup:x:34:
	operator:x:37:
	list:x:38:
	irc:x:39:
	src:x:40:
	gnats:x:41:
	shadow:x:42:
	utmp:x:43:
	video:x:44:
	staff:x:50:
	games:x:60:
	users:x:100:
	nogroup:x:65534:
	felinos:x:1000:
	docker:x:999:
	input:x:998:
	render:x:997:
	kvm:x:996:
	netdev:x:995:
	EOF

	# /etc/shadow (senhas vazias - definir com passwd)
	@cat > $(SYSROOT)/etc/shadow <<- 'EOF'
	root::0:0:99999:7:::
	daemon:*:0:0:99999:7:::
	bin:*:0:0:99999:7:::
	sys:*:0:0:99999:7:::
	sync:*:0:0:99999:7:::
	games:*:0:0:99999:7:::
	man:*:0:0:99999:7:::
	lp:*:0:0:99999:7:::
	mail:*:0:0:99999:7:::
	news:*:0:0:99999:7:::
	uucp:*:0:0:99999:7:::
	proxy:*:0:0:99999:7:::
	www-data:*:0:0:99999:7:::
	backup:*:0:0:99999:7:::
	list:*:0:0:99999:7:::
	irc:*:0:0:99999:7:::
	gnats:*:0:0:99999:7:::
	nobody:*:0:0:99999:7:::
	felinos::0:0:99999:7:::
	EOF
	@chmod 600 $(SYSROOT)/etc/shadow

	# /etc/fstab
	@cat > $(SYSROOT)/etc/fstab <<- 'EOF'
	# <device>  <mount>  <type>  <options>           <dump> <pass>
	/dev/vda    /        ext4    defaults,noatime    0      1
	proc        /proc    proc    defaults            0      0
	sysfs       /sys     sysfs   defaults            0      0
	devtmpfs    /dev     devtmpfs mode=0755          0      0
	tmpfs       /run     tmpfs   defaults,nosuid     0      0
	tmpfs       /tmp     tmpfs   defaults,nosuid     0      0
	EOF

	# /etc/inittab para busybox init
	@cat > $(SYSROOT)/etc/inittab <<- 'EOF'
	# FelinOS inittab for busybox init
	# Format: <id>:<runlevels>:<action>:<process>
	
	# System initialization
	::sysinit:/etc/init.d/rcS
	
	# Start getty on console
	ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
	tty1::respawn:/sbin/getty 38400 tty1
	
	# Shell on tty2-4
	tty2::respawn:/sbin/getty 38400 tty2
	tty3::respawn:/sbin/getty 38400 tty3
	tty4::respawn:/sbin/getty 38400 tty4
	
	# Shutdown
	::ctrlaltdel:/sbin/reboot
	::shutdown:/etc/init.d/rcK
	::restart:/sbin/init
	EOF

	# /etc/init.d/rcS - System startup script
	@mkdir -p $(SYSROOT)/etc/init.d
	@cat > $(SYSROOT)/etc/init.d/rcS <<- 'EOF'
	#!/bin/sh
	# FelinOS system startup script
	
	echo "Starting FelinOS..."
	
	# Mount filesystems from fstab
	/bin/mount -a
	
	# Set hostname
	/bin/hostname -F /etc/hostname 2>/dev/null || /bin/hostname felinos
	
	# Configure network interface (DHCP)
	if command -v udhcpc >/dev/null 2>&1; then
	    /sbin/udhcpc -i eth0 -n -q -s /usr/share/udhcpc/default.script &
	fi
	
	# Start syslog
	/sbin/syslogd -C 64
	/sbin/klogd
	
	# Seed random
	/bin/dd if=/dev/urandom of=/var/lib/urandom/seed count=1 bs=512 2>/dev/null || true
	
	# Load kernel modules (if any)
	/sbin/modprobe -a 2>/dev/null || true
	
	# Set up mdev for device management
	echo /sbin/mdev > /proc/sys/kernel/hotplug
	/sbin/mdev -s
	
	echo "FelinOS startup complete."
	EOF
	@chmod +x $(SYSROOT)/etc/init.d/rcS

	# /etc/init.d/rcK - Shutdown script
	@cat > $(SYSROOT)/etc/init.d/rcK <<- 'EOF'
	#!/bin/sh
	# FelinOS shutdown script
	
	echo "Shutting down FelinOS..."
	
	# Kill all processes
	/sbin/killall5 -15
	sleep 2
	/sbin/killall5 -9
	
	# Unmount filesystems
	/bin/umount -a -r
	
	echo "System halted."
	EOF
	@chmod +x $(SYSROOT)/etc/init.d/rcK

	# /etc/profile - Global environment
	@cat > $(SYSROOT)/etc/profile <<- 'EOF'
	# FelinOS global profile
	
	export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
	export PS1='\u@\h:\w\$ '
	export TERM=vt100
	export HISTSIZE=1000
	export HISTFILESIZE=2000
	
	# Aliases
	alias ls='ls --color=auto'
	alias ll='ls -l'
	alias la='ls -la'
	alias l='ls -CF'
	alias grep='grep --color=auto'
	alias df='df -h'
	alias du='du -h'
	alias free='free -h'
	
	# Editor
	export EDITOR=vi
	export VISUAL=vi
	
	# Locale
	export LANG=C.UTF-8
	export LC_ALL=C.UTF-8
	
	# FelinOS specific
	export FELINOS_VERSION=0.2
	export KERNEL_VERSION=0.2
	EOF

	# /etc/hostname
	@echo "felinos" > $(SYSROOT)/etc/hostname

	# /etc/hosts
	@cat > $(SYSROOT)/etc/hosts <<- 'EOF'
	127.0.0.1   localhost
	::1         localhost ip6-localhost ip6-loopback
	ff02::1     ip6-allnodes
	ff02::2     ip6-allrouters
	127.0.1.1   felinos
	EOF

	# /etc/resolv.conf (será atualizado por DHCP)
	@cat > $(SYSROOT)/etc/resolv.conf <<- 'EOF'
	nameserver 10.0.2.3
	search localdomain
	EOF

	# /etc/motd
	@cat > $(SYSROOT)/etc/motd <<- 'EOF'
	
	  __  __       _ _   _ 
	 |  \/  | __ _| | | | |
	 | |\/| |/ _` | | | | |
	 | |  | | (_| | | |_| |
	 |_|  |_|\__,_|_|\___/ 
	
	FelinOS 0.2  -  Gato kernel 0.2 (x86_64)
	
	Welcome to FelinOS!
	Type 'help' for available commands.
	
	EOF

	# /etc/shells
	@cat > $(SYSROOT)/etc/shells <<- 'EOF'
	/bin/sh
	/bin/ash
	/usr/bin/sh
	/usr/bin/ash
	EOF

	# /etc/inputrc (readline config)
	@cat > $(SYSROOT)/etc/inputrc <<- 'EOF'
	# FelinOS inputrc for ash/readline
	set editing-mode vi
	set keymap vi
	set convert-meta off
	set meta-flag on
	set output-meta on
	set show-all-if-ambiguous on
	set completion-ignore-case on
	EOF

	# /etc/mdev.conf - Device management rules
	@cat > $(SYSROOT)/etc/mdev.conf <<- 'EOF'
	# FelinOS mdev config
	# <device regex> <uid>:<gid> <mode> [>|=]<command>
	
	# Block devices
	hd[a-z]      root:disk  660  >/dev/hd
	sd[a-z]      root:disk  660  >/dev/sd
	vd[a-z]      root:disk  660  >/dev/vd
	nvme[0-9]n[0-9]p[0-9]  root:disk  660  >/dev/nvme
	loop[0-9]    root:disk  660  >/dev/loop
	ram[0-9]     root:disk  660  >/dev/ram
	
	# TTYs
	tty[0-9]     root:tty   660
	pty.*        root:tty   660
	console      root:tty   600
	
	# Network
	eth[0-9]     root:netdev 660
	tun[0-9]     root:netdev 660
	tap[0-9]     root:netdev 660
	
	# Misc
	null         root:root  666
	zero         root:root  666
	full         root:root  666
	random       root:root  666
	urandom      root:root  666
	tty          root:tty   666
	ptmx         root:tty   666
	kmsg         root:root  644
	rtc[0-9]     root:root  644
	EOF

	# /usr/share/udhcpc/default.script - DHCP client script
	@mkdir -p $(SYSROOT)/usr/share/udhcpc
	@cat > $(SYSROOT)/usr/share/udhcpc/default.script <<- 'EOF'
	#!/bin/sh
	# FelinOS udhcpc script
	
	[ -z "$1" ] && echo "Error: should be called from udhcpc" && exit 1
	
	RESOLV_CONF="/etc/resolv.conf"
	
	case "$1" in
	    deconfig)
	        /sbin/ifconfig $interface 0.0.0.0
	        ;;
	    renew|bound)
	        /sbin/ifconfig $interface $ip netmask $subnet broadcast $broadcast
	        
	        if [ -n "$router" ]; then
	            while route del default gw 0.0.0.0 dev $interface 2>/dev/null; do :; done
	            for i in $router; do
	                route add default gw $i dev $interface
	            done
	        fi
	        
	        # Update resolv.conf
	        > $RESOLV_CONF
	        [ -n "$domain" ] && echo "search $domain" >> $RESOLV_CONF
	        for i in $dns; do
	            echo "nameserver $i" >> $RESOLV_CONF
	        done
	        ;;
	esac
	
	exit 0
	EOF
	@chmod +x $(SYSROOT)/usr/share/udhcpc/default.script

	# /etc/os-release
	@cat > $(SYSROOT)/etc/os-release <<- 'EOF'
	NAME="FelinOS"
	VERSION="0.2"
	ID=felinos
	ID_LIKE=linux
	PRETTY_NAME="FelinOS 0.2"
	VERSION_ID="0.2"
	HOME_URL="https://github.com/felinos/felinos"
	SUPPORT_URL="https://github.com/felinos/felinos/issues"
	BUG_REPORT_URL="https://github.com/felinos/felinos/issues"
	EOF

	# /etc/issue
	@cat > $(SYSROOT)/etc/issue <<- 'EOF'
	FelinOS 0.2 (Gato kernel 0.2) \n \l
	EOF

	# Create user home directory
	@mkdir -p $(SYSROOT)/home/felinos
	@cp $(SYSROOT)/etc/profile $(SYSROOT)/home/felinos/.profile 2>/dev/null || true
	@echo "export PATH=\"/usr/local/bin:/usr/bin:/bin\"" > $(SYSROOT)/home/felinos/.bashrc 2>/dev/null || true

	# Create /var/lib/urandom for random seed
	@mkdir -p $(SYSROOT)/var/lib/urandom

	# /etc/securetty - TTYs where root can login
	@cat > $(SYSROOT)/etc/securetty <<- 'EOF'
	console
	tty1
	tty2
	tty3
	tty4
	ttyS0
	ttyS1
	EOF

	# /etc/ssh/sshd_config minimal (para quando openssh estiver instalado)
	@mkdir -p $(SYSROOT)/etc/ssh
	@cat > $(SYSROOT)/etc/ssh/sshd_config <<- 'EOF'
	Port 22
	Protocol 2
	HostKey /etc/ssh/ssh_host_rsa_key
	HostKey /etc/ssh/ssh_host_ecdsa_key
	HostKey /etc/ssh/ssh_host_ed25519_key
	PermitRootLogin yes
	PubkeyAuthentication yes
	PasswordAuthentication yes
	PermitEmptyPasswords no
	ChallengeResponseAuthentication no
	UsePAM no
	X11Forwarding yes
	PrintMotd yes
	ClientAliveInterval 300
	ClientAliveCountMax 2
	Subsystem sftp /usr/lib/ssh/sftp-server
	EOF

	# Gerar chaves SSH (placeholder - será feito no primeiro boot)
	@touch $(SYSROOT)/etc/ssh/ssh_host_rsa_key
	@touch $(SYSROOT)/etc/ssh/ssh_host_ecdsa_key
	@touch $(SYSROOT)/etc/ssh/ssh_host_ed25519_key
	@chmod 600 $(SYSROOT)/etc/ssh/ssh_host_*_key

	# /etc/nsswitch.conf
	@cat > $(SYSROOT)/etc/nsswitch.conf <<- 'EOF'
	passwd:     files
	group:      files
	shadow:     files
	hosts:      files dns
	networks:   files
	protocols:  files
	services:   files
	ethers:     files
	rpc:        files
	netgroup:   files
	EOF

	# /etc/protocols
	@cat > $(SYSROOT)/etc/protocols <<- 'EOF'
	ip      0       IP
	icmp    1       ICMP
	igmp    2       IGMP
	tcp     6       TCP
	udp     17      UDP
	ipv6    41      IPv6
	EOF

	# /etc/services
	@cat > $(SYSROOT)/etc/services <<- 'EOF'
	echo        7/tcp
	echo        7/udp
	discard     9/tcp           sink null
	discard     9/udp           sink null
	daytime     13/tcp
	daytime     13/udp
	qotd        17/tcp          quote
	ftp-data    20/tcp
	ftp         21/tcp
	ssh         22/tcp
	telnet      23/tcp
	smtp        25/tcp          mail
	domain      53/tcp          nameserver
	domain      53/udp          nameserver
	tftp        69/udp
	http        80/tcp          www www-http
	pop3        110/tcp
	ntp         123/udp
	https       443/tcp
	EOF

	# /etc/ld.so.conf (para musl não precisa, mas mantemos compatibilidade)
	@cat > $(SYSROOT)/etc/ld.so.conf <<- 'EOF'
	/usr/lib
	/usr/local/lib
	/lib
	/lib64
	EOF

	# /etc/ld.so.cache placeholder
	@touch $(SYSROOT)/etc/ld.so.cache

	# /root/.profile
	@cp $(SYSROOT)/etc/profile $(SYSROOT)/root/.profile

	# /root/.bashrc
	@cat > $(SYSROOT)/root/.bashrc <<- 'EOF'
	# FelinOS root bashrc
	source /etc/profile
	alias ls='ls --color=auto'
	alias ll='ls -l'
	alias la='ls -la'
	EOF

	# /var/log/wtmp, /var/log/btmp, /var/log/lastlog
	@touch $(SYSROOT)/var/log/wtmp $(SYSROOT)/var/log/btmp $(SYSROOT)/var/log/lastlog
	@chmod 644 $(SYSROOT)/var/log/wtmp $(SYSROOT)/var/log/lastlog
	@chmod 600 $(SYSROOT)/var/log/btmp

	@echo "  Base filesystem layout installed."

clean:
	@rm -rf $(SYSROOT)/bin $(SYSROOT)/sbin $(SYSROOT)/lib $(SYSROOT)/lib64 \
	        $(SYSROOT)/usr $(SYSROOT)/etc $(SYSROOT)/dev $(SYSROOT)/proc \
	        $(SYSROOT)/sys $(SYSROOT)/run $(SYSROOT)/tmp $(SYSROOT)/var \
	        $(SYSROOT)/root $(SYSROOT)/home $(SYSROOT)/boot $(SYSROOT)/mnt \
	        $(SYSROOT)/media $(SYSROOT)/srv

distclean: clean