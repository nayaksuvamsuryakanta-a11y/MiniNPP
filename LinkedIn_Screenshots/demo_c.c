#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    int score;
} DemoRecord;

static int calculate_score(const DemoRecord *record)
{
    if (!record) return 0;
    return record->score * 10;
}

static void print_record(const DemoRecord *record)
{
    if (!record) return;
    printf("%s: %d\n", record->name, calculate_score(record));
}

static int run_demo(void)
{
    DemoRecord records[] = {
        {"Syntax", 10},
        {"Folding", 9},
        {"Plugins", 10}
    };
    size_t count = sizeof(records) / sizeof(records[0]);

    for (size_t i = 0; i < count; ++i) {
        print_record(&records[i]);
    }

    return (int)count;
}

int main(void)
{
    // MiniNPP screenshot demo: structs, comments, functions, and foldable blocks.
    puts("MiniNPP visual demo");
    return run_demo();
}
