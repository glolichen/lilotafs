import os
import random

PRINT_EVERY = 10
REPEATS = 10000

def main():
    text = ""
    for i in range(REPEATS):
        seed = random.randrange(0, 1 << 31)
        os.system("make new_disk > /dev/null 2> /dev/null")
        code = os.system(f"./torture disk.bin {seed} > /dev/null")

        if i % PRINT_EVERY == PRINT_EVERY - 1:
            print("\b" * len(text), end="", flush=True)
            print(" " * len(text), end="", flush=True)
            print("\b" * len(text), end="", flush=True)
            text = f"{i + 1} / {REPEATS}"
            print(text, end="", flush=True)

if __name__ == "__main__":
    main()

