CREATE TABLE AISInput(
  T timestamp,
  TypeOfMobile varchar(50),
  MMSI integer,
  Latitude float,
  Longitude float,
  navigationalStatus varchar(50),
  ROT float,
  SOG float,
  COG float,
  Heading integer,
  IMO varchar(50),
  Callsign varchar(50),
  Name varchar(100),
  ShipType varchar(50),
  CargoType varchar(100),
  Width float,
  Length float,
  TypeOfPositionFixingDevice varchar(50),
  Draught float,
  Destination varchar(50),
  ETA varchar(50),
  DataSourceType varchar(50),
  SizeA float,
  SizeB float,
  SizeC float,
  SizeD float,
  Geom geometry(Point, 4326)
);


COPY AISInput(T, TypeOfMobile, MMSI, Latitude, Longitude, NavigationalStatus,
  ROT, SOG, COG, Heading, IMO, CallSign, Name, ShipType, CargoType, Width, Length,
  TypeOfPositionFixingDevice, Draught, Destination, ETA, DataSourceType,
  SizeA, SizeB, SizeC, SizeD, Geom)
FROM '/tmp/ais-all.csv' DELIMITER  ',' CSV HEADER;

UPDATE AISInput2 SET
  NavigationalStatus = CASE NavigationalStatus WHEN 'Unknown value' THEN NULL END,
  IMO = CASE IMO WHEN 'Unknown' THEN NULL END,
  ShipType = CASE ShipType WHEN 'Undefined' THEN NULL END,
  TypeOfPositionFixingDevice = CASE TypeOfPositionFixingDevice
  WHEN 'Undefined' THEN NULL END,
  Geom = ST_SetSRID( ST_MakePoint( Longitude, Latitude ), 4326);


CREATE TABLE AISInputFiltered AS
SELECT DISTINCT ON(MMSI,T) *
FROM AISInput
WHERE Longitude BETWEEN -16.1 and 32.88 AND Latitude BETWEEN 40.18 AND 84.17;

ALTER TABLE AISInputFiltered ADD COLUMN Geom3 geometry;
UPDATE AISInputFiltered 
SET Geom3 = ST_Transform(ST_SetSRID( ST_MakePoint( Longitude, Latitude ), 4326),25832);

CREATE TABLE Ships(MMSI, Trip, SOG, COG) AS
SELECT MMSI,
  tgeompoint_seq(array_agg(tgeompoint_inst( ST_Transform(Geom, 25832), T) ORDER BY T)),
  tfloat_seq(array_agg(tfloat_inst(SOG, T) ORDER BY T) FILTER (WHERE SOG IS NOT NULL)),
  tfloat_seq(array_agg(tfloat_inst(COG, T) ORDER BY T) FILTER (WHERE COG IS NOT NULL))
FROM AISInputFiltered
GROUP BY MMSI;

ALTER TABLE Ships ADD COLUMN Traj geometry;
UPDATE Ships SET Traj= trajectory(Trip);

-- check size of trips
WITH buckets (bucketNo, RangeKM) AS (
  SELECT 1, floatspan '[0, 0]' UNION
  SELECT 2, floatspan '(0, 50)' UNION
  SELECT 3, floatspan '[50, 100)' UNION
  SELECT 4, floatspan '[100, 200)' UNION
  SELECT 5, floatspan '[200, 500)' UNION
  SELECT 6, floatspan '[500, 1500)' UNION
  SELECT 7, floatspan '[1500, 10000)' ),
histogram AS (
  SELECT bucketNo, RangeKM, count(MMSI) as freq
  FROM buckets left outer join Ships on (length(Trip)/1000) <@ RangeKM
  GROUP BY bucketNo, RangeKM
  ORDER BY bucketNo, RangeKM
)
SELECT bucketNo, RangeKM, freq,
repeat('X', ( freq::float / max(freq) OVER () * 30 )::int ) AS bar FROM histogram;

-- clean trips that are too short or too long
DELETE FROM Ships
WHERE length(Trip) = 0 OR length(Trip) >= 1500000;

--///////////////////////////////////////////////////////////////////////////////////////////////////////////

-- separate trips that are near the ports atSTbox

CREATE TABLE ShipsNearPort(MMSI, Trip, SOG, COG, traj) AS
select S.MMSI, atStbox(S.trip,'SRID=25832;STBOX X((644339,6042108),(651422,6058548))'), S.SOG, S.COG, S.Traj
from Ships S
where eintersects(S.Trip, ST_MakeEnvelope(644339, 6042108, 651422, 6058548, 25832));

-- create ferries table
CREATE TABLE Ferries(MMSI, Trip, SOG, COG, Traj) AS
WITH Ports(Rodby, Puttgarden) AS (
  SELECT ST_MakeEnvelope(651135, 6058230, 651422, 6058548, 25832),
    ST_MakeEnvelope(644339, 6042108, 644896, 6042487, 25832)
)
SELECT S.MMSI, S.Trip, S.SOG, S.COG, S.Traj
FROM Ports P, ShipsNearPort S
WHERE eintersects(S.Trip, P.Rodby) AND eintersects(S.Trip, P.Puttgarden);

--///////////////////////////////////////////////////////////////////////////////////////////////////////////
-- Split trajectories into trips at ports with atGeometry

-- Calculate the bounding boxes for the ports
SELECT ST_MakeEnvelope(651135, 6058230, 651422, 6058548, 25832) AS Rodby,
       ST_MakeEnvelope(644339, 6042108, 644896, 6042487, 25832) AS Puttgarden
INTO TEMPORARY TABLE temp_port_boxes;


create table tripsIn(MMSI,timestamp) AS
SELECT F.MMSI, 
  startTimestamp(unnest(sequences(atGeometry(F.trip,P.Rodby)))) as timestamp
FROM Ferries F, temp_port_boxes P
order by MMSI,timestamp;

create table tripsOut(MMSI,timestamp) AS
SELECT F.MMSI, 
  startTimestamp(unnest(sequences(atGeometry(F.trip,P.Puttgarden)))) as timestamp
FROM Ferries F, temp_port_boxes P
order by MMSI,timestamp;


create table tripsSplitFerries(MMSI, trip) AS
select f.MMSI, 
       (CASE WHEN I.timestamp > O.timestamp then attime(F.trip, span(O.timestamp,I.timestamp))
            when I.timestamp < O.timestamp then attime(F.trip, span(I.timestamp,O.timestamp)) END) AS trip
from tripsIn I, tripsOut O, ferries F
where I.MMSI = F.MMSI and O.MMSI = I.MMSI;

------- ////////////

CREATE TABLE tripsIn(MMSI, timestamp, next_timestamp) AS
WITH trip_sequences AS (
  SELECT 
    F.MMSI, 
    startTimestamp(unnest(sequences(atGeometry(F.trip,P.Rodby)))) AS timestamp
  FROM Ferries F, temp_port_boxes P
  ORDER BY F.MMSI, startTimestamp(unnest(sequences(atGeometry(F.trip,P.Rodby))))
)
SELECT 
  MMSI, 
  timestamp, 
  LEAD(timestamp) OVER (PARTITION BY MMSI ORDER BY timestamp)
FROM trip_sequences;

CREATE TABLE tripsOut(MMSI, timestamp, next_timestamp) AS
WITH trip_sequences AS (
  SELECT 
    F.MMSI, 
    startTimestamp(unnest(sequences(atGeometry(F.trip,P.Puttgarden)))) AS timestamp
  FROM Ferries F, temp_port_boxes P
  ORDER BY F.MMSI, startTimestamp(unnest(sequences(atGeometry(F.trip,P.Puttgarden))))
)
SELECT 
  MMSI, 
  timestamp, 
  LEAD(timestamp) OVER (PARTITION BY MMSI ORDER BY timestamp)
FROM trip_sequences;

CREATE TABLE tripsSplitFerries(MMSI, trip) AS
SELECT F.MMSI, 
  attime(F.trip, span(I.timestamp, COALESCE(I.next_timestamp, O.timestamp))) as trip
FROM tripsIn I
INNER JOIN tripsOut O ON I.MMSI = O.MMSI AND O.timestamp >= I.timestamp
INNER JOIN ferries F ON I.MMSI = F.MMSI
ORDER BY F.MMSI, I.timestamp;



--///////////////////////////////////////////////////////////////////////////////////////////////////////////
-- Split trajectories into trips at ports with atStops

SELECT ST_MakeEnvelope(651135, 6058230, 651422, 6058548, 25832) AS Rodby,
       ST_MakeEnvelope(644339, 6042108, 644896, 6042487, 25832) AS Puttgarden
INTO TEMPORARY TABLE temp_port_boxes;


create table tripsAtStops(MMSI, trip,timestamp) AS
SELECT F.MMSI, 
  startSequence(stops(F.Trip, 25, '0 minutes')) as trip,
  startTimestamp(unnest(sequences(atGeometry(F.trip,P.Rodby)))) as timestamp
FROM Ferries F, temp_port_boxes P
order by timestamp;



--///////////////////////////////////////////////////////////////////////////////////////////////////////////

-- total distance
SELECT SUM( length( Trip ) ) FROM Ships;
SELECT length( Trip ) FROM Ships;

-- average speed
SELECT ABS(twavg(SOG) * 1.852 - twavg(speed(Trip))* 3.6 ) SpeedDifference
FROM Ships
ORDER BY SpeedDifference DESC;

-- average course
SELECT ABS(twavg(COG) - twavg(azimuth(Trip)) * 180.0/pi() ) AzimuthDifference
FROM Ships
ORDER BY AzimuthDifference DESC;

--///////////////////////////////////////////////////////////////////////////////////////////////////////////

-- ships that get close to ferries
CREATE TABLE BeltShips(MMSI, Trip, SOG, COG, Traj) AS
SELECT S.MMSI, S.Trip, S.SOG, S.COG, S.Traj
FROM ships S
WHERE NOT EXISTS (SELECT 1 FROM Ferries F WHERE F.MMSI = S.mmSI and F.Trip = S.Trip);

create table NearMiss(MMSI, Trip, SOG, COG, Traj) AS
SELECT S.MMSI, F.MMSI, S.Trip, F.trip, shortestLine(S.trip, F.trip)
FROM BeltShips S, Ferries F
WHERE S.MMSI <> F.MMSI AND
edwithin(S.Trip, F.Trip, 300);
