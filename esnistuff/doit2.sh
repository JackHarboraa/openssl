#!/bin/bash

#set -x

# try feed 2 ESNIKeys into a test

# Two 0xff01 instances...
#ESNI1="/wEhoY5aACQAHQAgHqTcPWLSyVnFusv84efGXK4JIC/oPRSs/va4mI661QUAAhMBAQQAAAAAW/XVYAAAAABb/b5gAAA="
#ESNI2="/wGmmCEcACQAHQAgY2O0imWb0aAELxQ99Ibv76BcFvRPH0Eit9ZZ8+SKb20AAhMBAQQAAAAAXJowYAAAAABcohlgAAA="
#ESNI="$ESNI1;$ESNI2"
# Two 0xff02 instances...
#ESNI="FF02FF93090D000B6578616D706C652E636F6D0024001D00202857EF701013510D270E531232C40A09226A83391919F4ED3F6B3D08547A7F68000213010104000000005C93BA56000000005C9CF4D60000FF027CE3FD9C000B6578616D706C652E6E65740024001D00208C48CF4B00BAAF1191C8B882CFA43DC7F45796C7A0ADC9EB6329BE25B9464235000213010104000000005C9588C7000000005C9EC3470000"
# one ff02 with one extension
#ESNI="ff0228b94458000e726573706f6e7369626c652e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca475a0000000005cadb020001a1001001604b918e9d3062a042e0000010067000000000000000a"
# and another one ff02 with one extension
#ESNI="ff0235b744110006666f6f2e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca4762c000000005cadb0ac001a1001001604b918e977062a042e0000010014000000000000000a"
# two ff02's with one extension
#ESNI="ff0228b94458000e726573706f6e7369626c652e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca475a0000000005cadb020001a1001001604b918e9d3062a042e0000010067000000000000000aff0235b744110006666f6f2e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca4762c000000005cadb0ac001a1001001604b918e977062a042e0000010014000000000000000a"
# an ESNI with some crap extensions (greased)
#ESNI="ff0238ed29ea0006666f6f2e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca4aeb8000000005cade93800befff1003ecdc23775802e5360298901b92e140baf87fdcbc8b114fd4651ba9b76499b627cc67077c11792bc8a5da2c60189b49f63fccee0bd56d172b03fafc32fc30b1001001604b918e977062a042e0000010014000000000000000afff2005ef65696177cfe99e9c6cabf016a38bb12b958a74203c59caad88ca9fca60333fc0fb78764ccfaa39a41cadd535929d4dd76504d15acdef44ccc3b2b0148be5b665c115045586ea47862a37ca95cac6b5a32debe931bb6be4e9dae05315b0c"
# an ESNI with some crap extensions (greased) incl. one that's got an empty value
ESNI="ff023bc449be0006666f6f2e69650024001d0020c01e55e8801e772487553cf6d0fc80e9887a58a7cf8a5361de0a0aca28a8b874000213010104000000005ca4b35d000000005cadeddd0078fff30000fff10009c77963690088e3121a1001001604b918e977062a042e0000010014000000000000000afff30000fff20045a44ec709ebf763ba173374ba2fa41e26f58c5c39539975bba7d342d94bef9145cee7939ab87ec1c1d2010d537189cde0e8c7e598adb49330a72d065dd957ea293a726b52b2"
HIDDEN="encryptedsni.com"
COVER="www.cloudflare.com"

# ASCII Hex of 1st private key in nss.ssl.debug, eliminate spaces etc.
PRIV="29ab54e6258de21b4178a6270db88ad411809199c267a6317646728966fdca02"

# H/S key share - from AAD in nss.out
HSKS="a8cc84eed13d54f62e69d269988d79ef0514f6a8e64dcb774369f2eff560b12b"

# Client_random
CRND="62ea83d6f9f946248fa41b29f0127e72a0aeadce44262bed399f2fc4a8365e0b"

# Nonce
NONCE="45a61b547439b11dac1274e301145084"

# should really add getopt but this is likely short-term (famous last
# words those:-)

if [[ "$1" == "fresh" ]]
then
	echo "Checking for fresh ESNI value from $HIDDEN"
	ESNI=`dig +short TXT _esni.$HIDDEN | sed -e 's/"//g'`	
	echo "Fresh ESNI value: $ESNI"
fi	

# CRYPT_INTEROP Version
#valgrind --leak-check=full ./esni -s $HIDDEN -f $COVER -e $ESNI -p $PRIV -r $CRND -k $HSKS -n $NONCE

# "normal" version - doesn't take other folks' internal crypto inputs
valgrind --leak-check=full ./esni -s $HIDDEN -f $COVER -e "$ESNI"
