# cse321-journal-management-system
# CSE321 Journaling Filesystem

A C programming project developed as part of my undergraduate **CSE321 course project**.

## About the Project

This project implements a simplified journaling filesystem using the C programming language. It works with a filesystem image and demonstrates how filesystem changes can be recorded in a journal before being applied to the filesystem.

## Features

- Create new files in the filesystem
- Manage filesystem metadata
- Work with inodes and directory entries
- Manage inode and data block bitmaps
- Record filesystem changes in a journal
- Commit filesystem transactions
- Install/replay committed journal transactions

## Technologies

- **C Programming Language**
- File I/O
- POSIX system calls
- Structures and pointers
- Filesystem management
- Journaling and transaction concepts

## How It Works

The program uses a filesystem image named `vsfs.img`.

The `create` command creates a new file and records the required filesystem changes in the journal.

```bash
./journal create <filename>
```

The `install` command applies committed journal transactions to the filesystem.

```bash
./journal install
```

## Compilation

The program can be compiled using GCC:

```bash
gcc journal.c -o journal
```

## Academic Information

**Course:** CSE321  
**Project Type:** Undergraduate Course Project  
**Language:** C

This project was developed as part of my undergraduate coursework and is included in my GitHub portfolio for academic and learning purposes.
