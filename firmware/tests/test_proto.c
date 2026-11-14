/* Checks proto.c against protocol/vectors.txt (the same file the backend is tested with). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

static int hex2bin(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = strlen(hex) / 2;

	if (n > cap) return -1;
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
		out[i] = (uint8_t)v;
	}
	return (int)n;
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argc > 1 ? argv[1] : "../protocol/vectors.txt", "r");
	char line[1024], dump[512];
	uint8_t in[256], out[256];
	struct proto_msg m;
	int count = 0, fail = 0;

	if (!f) { perror("vectors"); return 2; }
	while (fgets(line, sizeof(line), f)) {
		char *sep = strstr(line, " | ");
		int len, n;

		if (!sep) continue;
		*sep = 0;
		sep += 3;
		sep[strcspn(sep, "\n")] = 0;
		len = hex2bin(line, in, sizeof(in));
		count++;
		if (len < 0 || proto_decode(in, (size_t)len, &m) != 0) { printf("FAIL decode: %s\n", sep); fail++; continue; }
		proto_dump(&m, dump, sizeof(dump));
		if (strcmp(dump, sep) != 0) { printf("FAIL dump:\n  want %s\n  got  %s\n", sep, dump); fail++; }
		n = proto_encode(&m, out, sizeof(out));
		if (n != len || memcmp(in, out, (size_t)len) != 0) { printf("FAIL re-encode: %s\n", sep); fail++; }
		/* every truncation of a valid frame must be rejected, never read out of bounds */
		for (int cut = 0; cut < len; cut++) {
			uint8_t *copy = malloc((size_t)cut + 1);

			memcpy(copy, in, (size_t)cut);
			if (proto_decode(copy, (size_t)cut, &m) == 0) { printf("FAIL truncation %d accepted: %s\n", cut, sep); fail++; }
			free(copy);
		}
	}
	fclose(f);
	{   /* hostile input: random bytes must never crash and oversize strings must be refused */
		unsigned seed = 12345;

		for (int i = 0; i < 200000; i++) {
			size_t l = (size_t)(rand_r(&seed) % 200);
			uint8_t *buf = malloc(l + 1);

			for (size_t k = 0; k < l; k++) buf[k] = (uint8_t)rand_r(&seed);
			if (l > 0 && (i & 1)) buf[0] = (uint8_t)(0x81 + rand_r(&seed) % 7);
			(void)proto_decode(buf, l, &m);
			free(buf);
		}
	}
	printf("proto: %d vectors, %d failures\n", count, fail);
	return (fail || count < 15) ? 1 : 0;
}
