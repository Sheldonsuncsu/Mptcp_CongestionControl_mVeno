 All the following commands you need to run as root:
1	Put the file in the mptcp directory, like /usr/src/mptcp_2015_11_17/net/mptcp.
2	Change the file permission  with the commad  "chmod  777 mptcp0.9_new-mveno.c".
3	Modify the Makefile file:
      At the end of the file, add the sentence:
      obj-m +=mptcp_mveno.o
4	 Execute the command "make -C /usr/.src/mptcp_2015_11_17 M=$PWD modules ". It creates two output files mptcp_mveno.o and mptcp_mveno.ko.
5	Insert the mveno module into the kernel: Insmod mptcp_mveno.ko
