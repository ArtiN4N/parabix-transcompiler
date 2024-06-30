git reset --hard origin/master;
git clean -fxd; git pull; 
cd build; 
make latinascii; bin/latinascii ../lasciitest.txt --enable-illstrator; 
cd ../;