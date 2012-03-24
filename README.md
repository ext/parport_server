Parallel Port Interface Server
==============================

A simple parallel port server for controlling individual bits remotely. It features a simple protocol to set, toggle and strobe bits and can listen on both unix domain sockets and TCP connections.

It is useful when something is hooked to the parallel port and is going to be remotely controlled.

Usage
-----

**domain**

    ./parserver /dev/parport0
    echo "strobe d0 300" | socat stdin UNIX-CONNECT:parserver.sock

**TCP**

    ./parserver -l -p 7613 /dev/parport0
    echo "strobe d0 300" | nc localhost 7613

Protocol
--------

**SET PIN MODE**
Set  PIN to MODE where PIN is d0-7 and MODE is HIGH, LOW or TOGGLE.

**STROBE PIN TIME**
Set PIN high for TIME milliseconds and then back to low.

Install
-------

Installation is a simple `make` and `make install`.

Gentoo users can use layman to add ext-devlibs where an ebuild is present which includes an init.d script.
