# Student Record Management System

A system demonstrating object-oriented programming, file
handling, data processing, and real-time WebSocket communication — built entirely on
POSIX primitives with **no external libraries**.


## Building
mkdir -p build

# Server
g++ -std=c++17 -O2 -Iinclude -o build/server server_main.cpp -lpthread

# Client
g++ -std=c++17 -O2 -Iinclude -o build/client client_main.cpp -lpthread

# Tests
g++ -std=c++17 -O2 -Iinclude -o build/tests test_main.cpp -lpthread


## Running



### 1. Start the Server

cd build
./server --port 9001 --csv --input ../data/students_input.csv --output ../data/student_output.csv

Options:
- `--port <n>`  – WebSocket port (default: 9001)
- `--input <path>` – Path to input CSV file (default: data/students_input.csv)
- `--output <path>` – Path to output CSV file (default: data/students_ouput.csv)

### 2. Browser UI

Open `build/web/index.html` in any browser. The page will auto-connect to
`ws://127.0.0.1:9001`. All CRUD operations update in real-time.

### 3. C++ CLI Client

In a second terminal:

cd build
./client --host 127.0.0.1 --port 9001 --csv ../data/students_input.csv

The CLI client:
- Connects to the server
- Uploads the local CSV on startup
- Provides an interactive menu for CRUD operations
- Every operation is broadcast live to all connected clients

