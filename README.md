GravityCoin [GXX] (Sigma protocol) -- discontinued

===============================


[![Build Status](https://travis-ci.com/GravityCoinOfficial/GravityCoin.svg?branch=master)](https://travis-ci.com/GravityCoinOfficial/GravityCoin)

GravityCoin
----------------
* Coin Suffix: GXX
* Algorithm: lyra2z330
* Algo params: LYRA2(BEGIN(thash), 32, BEGIN(nVersion), 80, BEGIN(nVersion), 80, 2, 330, 256)
* Target Spacing: 150 Seconds
* Retarget: every block
* Confirmation: 6 Blocks
* Maturity: 120 Blocks
* Blocks: ~576 per day
* Total Coins: 9,999,999 GXX
* Min TX Fee: 0.001 GXX
* Block Size: 4MB

Net Parameters
----------------
* P2P Port=29100
* RPC Port=29200
* Client core=13.4
* Client name=GravityCoin.qt
* Conf file=GravityCoin.conf

Installation folder
----------------
* Windows: C:\Users\Username\AppData\Roaming\GravityCoin
* Mac: /Library/Application Support/GravityCoin
* Unix: /.GravityCoin

Debian/Ubuntu Linux Daemon Build Instructions
================================================

	install dependencies:
	Build a node or qt:

	if you need a swap memory:
	free
	dd if=/dev/zero of=/var/swap.img bs=2048 count=1048576
	mkswap /var/swap.img
	swapon /var/swap.img
	free


	sudo apt-get update
	sudo apt-get upgrade

	sudo apt-get install git build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils libboost-all-dev

	sudo apt-get install software-properties-common
	sudo add-apt-repository ppa:bitcoin/bitcoin
	sudo apt-get update
	sudo apt-get install libdb4.8-dev libdb4.8++-dev

	sudo apt-get install libminiupnpc-dev libzmq3-dev
	for qt:
	sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libqrencode-dev

	git clone https://github.com/GravityCoinOfficial/gravitycoin

	cd GravityCoin
	for vps:
	./autogen.sh
	./configure  --without-gui
	make -j 4   (-j is optional, number of your cores, -j 4)

	for qt:
	./autogen.sh
	./configure
	make -j 4   (-j is optional, number of your cores, -j 4)

	cd src
	strip GravityCoind
	strip GravityCoin-cli
	or:
	cd src
	cd qt
	strip GravityCoin-qt

	files are:
	GravityCoind
	GravityCoin-cli

	GravityCoin-qt
	GravityCoin.conf
	xnode.conf
	data folder:
	GravityCoin

	port 29100
	rpc port 29200

Example GravityCoin.conf Configuration
===================================================

	#listen=1
	#server=1
	#rpcpassword=
	#rpcusername=
	#maxconnections=16
	#connect=
	#addnode=
	#rescan=0
	#reindex=0
	#reindex-chainstate=0
	#xnode=1
	#xnodeprivkey=123123123123123123123123 ## Replace with your xnode private key
	#externalip=123.123.123.123:29100 ## Replace with your node external IP

