CC      = gcc
CFLAGS  = -Wall -O2 -Isrc
LDFLAGS = -lm

SRC = src/server.c       \
      src/bins.c         \
      src/vehicles.c     \
      src/drivers.c      \
      src/routes.c       \
      src/simulation.c   \
      src/storage.c      \
      src/validation.c   \
      src/auth.c         \
      src/heatmap.c      \
      src/citizen.c

TARGET = smart_waste

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run
