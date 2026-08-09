#include "Core/Utils/DateTime.h"

int main()
{
	const DateTime dateTime;

	if (dateTime.GetTime() == 0)
		return 1;
	if (dateTime.seconds < 0 || dateTime.seconds > 60)
		return 1;
	if (dateTime.minutes < 0 || dateTime.minutes > 59)
		return 1;
	if (dateTime.hours < 0 || dateTime.hours > 23)
		return 1;
	if (dateTime.day < 1 || dateTime.day > 31)
		return 1;
	if (dateTime.month < 0 || dateTime.month > 11)
		return 1;
	if (dateTime.weekday < 0 || dateTime.weekday > 6)
		return 1;
	if (dateTime.yearday < 0 || dateTime.yearday > 365)
		return 1;

	return 0;
}
