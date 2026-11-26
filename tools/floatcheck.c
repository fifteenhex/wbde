/*
 * Configure-time check for a gcc 16 m68k bug: at -O2 a float kept live
 * across a call is spilled as 80-bit extended (12 bytes) and reloaded as a
 * 4-byte single from the low bytes of the slot.  -ffloat-store avoids it,
 * and the build adds that flag when this program fails.  Reduced from
 * smolgl's glRotatef(): len is live across the sincos call.
 *
 * Exits 0 when the rotation matrix is right, 1 when it is garbage.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

volatile int enabled = 1;
static float result[16];

__attribute__((noipa)) static int active(void)
{
	return enabled;
}

__attribute__((noipa)) static void post(const float *m)
{
	int i;

	for (i = 0; i < 16; i++)
		result[i] = m[i];
}

__attribute__((noipa))
static void rot(float angle, float x, float y, float z)
{
	float m[16], len, s, c, t;
	double a = angle * (3.14159265358979323846 / 180.0);

	if (!active())
		return;
	len = sqrtf(x * x + y * y + z * z);
	if (len < 1e-12f)
		return;
	s = (float)sin(a);
	c = (float)cos(a);
	x /= len;
	y /= len;
	z /= len;
	t = 1.0f - c;
	m[0] = t * x * x + c;	 m[4] = t * x * y - s * z; m[8] = t * x * z + s * y;  m[12] = 0;
	m[1] = t * x * y + s * z; m[5] = t * y * y + c;	   m[9] = t * y * z - s * x;  m[13] = 0;
	m[2] = t * x * z - s * y; m[6] = t * y * z + s * x; m[10] = t * z * z + c;    m[14] = 0;
	m[3] = 0;		 m[7] = 0;		   m[11] = 0;		      m[15] = 1;
	post(m);
}

int main(int argc, char **argv)
{
	/* 90 degrees about z, given as (0, 0, 2) so len is 2 */
	static const float want[16] = { 0, 1, 0, 0,  -1, 0, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
	int i, bad = 0;

	rot(90.0f, 0.0f, 0.0f, 2.0f);
	for (i = 0; i < 16; i++)
		if (!(fabsf(result[i] - want[i]) < 1e-5f))
			bad++;
	if (bad || argc > 1) {
		for (i = 0; i < 16; i++)
			printf("%s%g", i % 4 ? " " : "\n", result[i]);
		printf("\n%s\n", bad ? "WRONG: a float spilled as extended was reloaded as single" : "ok");
	}
	return bad != 0;
}
