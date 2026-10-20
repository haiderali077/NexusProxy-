FROM gcc:14.2.0

WORKDIR /app

# Copy only the source file so unrelated changes do not invalidate this layer.
COPY server.cpp .

# Build the Linux executable when the image is created.
RUN g++ -std=c++20 -Wall -Wextra -Wpedantic server.cpp -o main

# Exec form forwards container signals directly to the program.
CMD ["./main"]
