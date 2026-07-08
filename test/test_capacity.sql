create table warehouse (w_id int, name char(8));
insert into warehouse values (1, 'a');
insert into warehouse values (2, 'a');
-- 插入到200条
insert into warehouse values (200, 'a');
create index warehouse(w_id);
select * from warehouse where w_id = 169;
select * from warehouse where w_id = 170;
drop index warehouse(w_id);
drop table warehouse;
