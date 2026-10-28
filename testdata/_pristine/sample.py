class Demo:
    """Small Python fixture for syntax and folding tests."""
    def run(self, values):
        for value in values:
            if value > 2:
                print(f"value={value}")

if __name__ == "__main__":
    Demo().run([1, 2, 3, 4])
