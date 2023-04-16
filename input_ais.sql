CREATE OR REPLACE FUNCTION input_ais()
RETURNS text AS $$
BEGIN

  DROP TABLE IF EXISTS AISInput;
  CREATE TABLE AISInput(
    T timestamp,
    TypeOfMobile varchar(100),
    MMSI integer,
    Latitude float,
    Longitude float,
    navigationalStatus varchar(100),
    ROT float,
    SOG float,
    COG float,
    Heading integer,
    IMO varchar(100),
    Callsign varchar(100),
    Name varchar(100),
    ShipType varchar(100),
    CargoType varchar(100),
    Width float,
    Length float,
    TypeOfPositionFixingDevice varchar(100),
    Draught float,
    Destination varchar(100),
    ETA varchar(100),
    DataSourceType varchar(100),
    SizeA float,
    SizeB float,
    SizeC float,
    SizeD float,
    Geom geometry(Point, 4326)
  );

  RAISE INFO 'Reading CSV files ...';

  COPY AISInput(T, TypeOfMobile, MMSI, Latitude, Longitude, NavigationalStatus,
    ROT, SOG, COG, Heading, IMO, CallSign, Name, ShipType, CargoType, Width, Length,
    TypeOfPositionFixingDevice, Draught, Destination, ETA, DataSourceType,
    SizeA, SizeB, SizeC, SizeD)
  FROM '/home/marianamgd/ppdaa.csv' DELIMITER ',' CSV HEADER;

  RAISE INFO 'Updating AISInput table ...';
  
  UPDATE AISInput SET
    NavigationalStatus = CASE NavigationalStatus WHEN 'Unknown value' THEN NULL END,
    IMO = CASE IMO WHEN 'Unknown' THEN NULL END,
    ShipType = CASE ShipType WHEN 'Undefined' THEN NULL END,
    TypeOfPositionFixingDevice = CASE TypeOfPositionFixingDevice
    WHEN 'Undefined' THEN NULL END,
    Geom = ST_SetSRID( ST_MakePoint( Longitude, Latitude ), 4326);

  RAISE INFO 'Creating AISInputFiltered table ...';

  DROP TABLE IF EXISTS AISInputFiltered;
  CREATE TABLE AISInputFiltered AS
  SELECT DISTINCT ON(MMSI,T) *
  FROM AISInput
  WHERE Longitude BETWEEN -16.1 and 32.88 AND Latitude BETWEEN 40.18 AND 84.17;

  RAISE INFO 'Creating Ships table ...';

  DROP TABLE IF EXISTS Ships;
  CREATE TABLE Ships(MMSI,T, Trip, SOG, COG) AS
  SELECT MMSI,T,
    tgeompoint_seq(array_agg(tgeompoint_inst(ST_Transform(Geom, 25832), T) ORDER BY T)),
    tfloat_seq(array_agg(tfloat_inst(SOG, T) ORDER BY T) FILTER (WHERE SOG IS NOT NULL)),
    tfloat_seq(array_agg(tfloat_inst(COG, T) ORDER BY T) FILTER (WHERE COG IS NOT NULL))
  FROM AISInputFiltered
  GROUP BY MMSI,T;

  ALTER TABLE Ships ADD COLUMN Traj geometry;
  UPDATE Ships SET Traj = trajectory(Trip);

  RETURN 'The End';
END;
$$ LANGUAGE 'plpgsql' STRICT;


 copy (select mmsi, Latitude, Longitude,t 
 from AISInput 
 where (Longitude < -16.1 or Longitude > 32.88) or(Latitude < 40.18 or Latitude >  84.17)) to '/Users/marianaduarte/Desktop/OutofRange.csv' DELIMITER ',' CSV HEADER;


 select count(*)
 from AISInput 
 where (Longitude < -16.1 or Longitude > 32.88) or(Latitude < 40.18 or Latitude >  84.17)



Copy (select round (angulardifference (trip)) from ships) to '/Users/marianaduarte/Desktop/angles.csv'  DELIMITER ',' CSV HEADER
