# Assignment 1

This directory contains the implementation for the CS6600 assignment 1 Simulator: Simple 5 Stage in-order Pipeline Processor.

## Build

From the `assignment1` folder:

```bash
make
```

## Run

On Windows (PowerShell or CMD):

```powershell
(echo n 100 & echo p & echo s & echo q) | pipesim.exe example.pisa
```

On macOS/Linux (bash/zsh):

```bash
printf "n 100\np\ns\nq\n" | ./pipesim example.pisa
```

Use `example.pisa` or your own `.pisa` input file in the `assignment1` folder.
