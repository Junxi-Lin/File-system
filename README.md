# File System Project (C)

## Overview
This project implements a basic file system in C on a Linux-based environment. 
It simulates core file system functionalities including file storage, directory 
management, and free space allocation.

## Features
- Volume initialization and formatting
- Free space allocation and management
- Directory structure creation and navigation
- File operations: create, read, write, delete
- Support for basic shell commands (ls, cp, mv, rm, touch, etc.)

## System Components
- File system initialization (fsInit)
- Directory management functions
- File I/O interface (b_open, b_read, b_write, b_close)
- Free space tracking system
- Logical block addressing using LBAread and LBAwrite

## Technologies
- C programming language
- Linux (Ubuntu)
- Makefile build system
- Low-level file system concepts

## My Contributions
- Implemented file system core functionalities including file creation and access
- Developed directory management and navigation logic
- Integrated file operations with logical block storage
- Tested and debugged system using custom shell interface

## What I Learned
- File system architecture and design
- Low-level storage management and logical block addressing
- Memory management and buffering techniques
- Working with system-level APIs in C
- Debugging complex system interactions

## How to Run
```bash
make
make run
