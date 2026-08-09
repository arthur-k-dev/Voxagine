#include "Framework/Check.h"

#include "Core/Utils/DateTime.h"

/* Every field of a default-constructed DateTime is inside the range its own
   documentation claims. Was a standalone executable with a hand-written main;
   it is a check now for the same reason everything else here is - one runner,
   one report, one way to add a case. */
VOXAGINE_CHECK(DateTime, DefaultConstructionIsTheCurrentTime)
{
	const DateTime dateTime;

	CHECK_NE(dateTime.GetTime(), 0);

	CHECK_GE(dateTime.seconds, 0);
	CHECK_LE(dateTime.seconds, 60);

	CHECK_GE(dateTime.minutes, 0);
	CHECK_LE(dateTime.minutes, 59);

	CHECK_GE(dateTime.hours, 0);
	CHECK_LE(dateTime.hours, 23);

	CHECK_GE(dateTime.day, 1);
	CHECK_LE(dateTime.day, 31);

	CHECK_GE(dateTime.month, 0);
	CHECK_LE(dateTime.month, 11);

	CHECK_GE(dateTime.weekday, 0);
	CHECK_LE(dateTime.weekday, 6);

	CHECK_GE(dateTime.yearday, 0);
	CHECK_LE(dateTime.yearday, 365);
}
