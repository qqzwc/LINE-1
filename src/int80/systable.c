#include "systable.h"
#include "arch\x86\unistd_32.h"





ULONG LinuxServiceTable[] = 
{
	NOIMLE,				//0
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//5
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//10
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//15
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//20
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//25
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//30
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//35
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//40
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//45
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//50
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//55
	NOIMLE,
	NOIMLE,
	NOIMLE,
	sys_oldolduname,
	NOIMLE,				//60
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//65
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//70
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//75
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//80
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//85
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//90
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//95
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//100
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//105
	NOIMLE,
	NOIMLE,
	NOIMLE,
	sys_olduname,
	NOIMLE,				//110
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//115
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//120
	NOIMLE,
	sys_uname,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//125
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//130
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//135
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//140
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//145
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//150
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//155
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//160
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//165
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//170
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//175
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//180
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//185
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//190
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//195
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//200
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//205
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//210
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//215
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//220
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//225
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//230
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//235
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//240
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//245
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//250
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//255
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//260
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//265
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//270
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//275
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//280
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//285
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//290
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//295
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//300
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//305
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//310
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//315
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//320
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//325
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//330
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//335
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//340
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//345
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//350
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//355
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//360
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//365
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//370
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//375
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//380
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//385
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//390
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//395
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,
	NOIMLE,				//400
	NOIMLE
};