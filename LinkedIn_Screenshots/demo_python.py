"""MiniNPP screenshot demo for Python syntax highlighting."""

class ScoreBoard:
    """Stores named scores and calculates a summary."""

    def __init__(self, scores):
        self.scores = scores

    def report(self):
        total = 0
        for name, score in self.scores.items():
            adjusted = score * 2 if score >= 8 else score
            print(f"{name}: {adjusted}")
            total += adjusted
        return total


def main():
    board = ScoreBoard({"Highlighting": 10, "Folding": 9, "Plugins": 8})
    average = board.report() / len(board.scores)
    print(f"Average: {average:.1f}")


if __name__ == "__main__":
    main()
