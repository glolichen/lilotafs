import os
import sys
import random

PRINT_EVERY = 1

def main():
    disk = sys.argv[1]
    text = ""
    i = 0
    while True:
        seed = random.randrange(0, 1 << 31)
        os.system(f"(dd if=/dev/zero bs=2M count=1 | tr '\\000' '\\377' > {disk}) > /dev/null 2> /dev/null")
        code = os.system(f"./torture {disk} {seed} > /dev/null")
        if code != 0:
            print(f"problem {seed}")

        if i % PRINT_EVERY == PRINT_EVERY - 1:
            print("\b" * len(text), end="", flush=True)
            print(" " * len(text), end="", flush=True)
            print("\b" * len(text), end="", flush=True)
            text = f"{i + 1}"
            print(text, end="", flush=True)

        i += 1

if __name__ == "__main__":
    main()

