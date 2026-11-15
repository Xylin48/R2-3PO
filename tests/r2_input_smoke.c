#include "r2_input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char *buffer = NULL;
    size_t capacity = 0, length = 0;

    FILE *stream = tmpfile();
    assert(stream != NULL);
    assert(fwrite("abc\n", 1, 4, stream) == 4);
    assert(fwrite("abcd\nok\n", 1, 8, stream) == 8);
    rewind(stream);

    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) == R2_INPUT_LINE_OK);
    assert(length == 3 && strcmp(buffer, "abc") == 0);

    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) == R2_INPUT_LINE_TOO_LONG);
    assert(length == 3);
    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) == R2_INPUT_LINE_OK);
    assert(length == 2 && strcmp(buffer, "ok") == 0);
    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) == R2_INPUT_LINE_EOF);
    fclose(stream);

    stream = tmpfile();
    assert(stream != NULL);
    {
        const unsigned char binary_line[] = {'a', '\0', 'b', '\n'};
        assert(fwrite(binary_line, 1, sizeof(binary_line), stream) == sizeof(binary_line));
    }
    rewind(stream);
    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) ==
           R2_INPUT_LINE_EMBEDDED_NUL);
    fclose(stream);

    stream = tmpfile();
    assert(stream != NULL);
    assert(fwrite("last", 1, 4, stream) == 4);
    rewind(stream);
    assert(r2_read_complete_line(stream, &buffer, &capacity, 3, &length) == R2_INPUT_LINE_TOO_LONG);
    fclose(stream);

    stream = tmpfile();
    assert(stream);
    const size_t large_length = 64000;
    for (size_t i = 0; i < large_length; ++i)
        assert(fputc(i == large_length - 1 ? ' ' : 'x', stream) != EOF);
    assert(fputs("\r\n", stream) >= 0);
    rewind(stream);
    assert(r2_read_complete_line(stream, &buffer, &capacity, large_length, &length) == R2_INPUT_LINE_OK);
    assert(length == large_length && capacity <= large_length + 2);
    assert(buffer[0] == 'x' && buffer[large_length - 2] == 'x' &&
           buffer[large_length - 1] == ' ' && buffer[large_length] == '\0');
    fclose(stream);
    free(buffer);

    puts("Bounded complete input reader tests passed.");
    return 0;
}
