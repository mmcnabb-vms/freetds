if exists (select 1 from sysobjects where type = 'U' and name = 'bcp_defaultdate') drop table bcp_defaultdate
go
CREATE TABLE bcp_defaultdate (
	  not_null_int			int NOT NULL
	, not_null_datetime		datetime DEFAULT GETDATE() NOT NULL
	, null_datetime			datetime DEFAULT GETDATE() NULL
)

INSERT bcp_defaultdate(not_null_int)
VALUES ( 12 )
go

select not_null_int, null_datetime, not_null_datetime from bcp_defaultdate
go

select not_null_int, null_datetime, not_null_datetime from bcp_defaultdate
go

drop table bcp_defaultdate
go
