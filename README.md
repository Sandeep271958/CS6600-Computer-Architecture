# CS6600-7-pipesim
This is a repo that will contain all of the code that's part of the CS6600 Computer Architecture course in the July to November 2026 Semester at IIT Madras under the guidance of Professor Madhu Muthyam

## Assignment 1
To build and run the assignment 1 implementation from the `assignment1` folder:

```bash
cd assignment1
make
```

On Windows (PowerShell or CMD):

```powershell
(echo n 100 & echo p & echo s & echo q) | pipesim.exe example.pisa
```

On macOS/Linux (bash/zsh):

```bash
printf "n 100\np\ns\nq\n" | ./pipesim example.pisa
```

This will compile the simulator and execute it using the example `example.pisa` input file.

## Contributors
- Mourya Sai Sandeep `EE22B045`
- Parthasarathi `EE22B089`
- Dattatreya Sastry `EE22B175`
