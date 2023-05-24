/*****************************************************************************
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 * Copyright (c) 2016-2023, Université libre de Bruxelles and MobilityDB
 * contributors
 *
 * MobilityDB includes portions of PostGIS version 3 source code released
 * under the GNU General Public License (GPLv2 or later).
 * Copyright (c) 2001-2023, PostGIS contributors
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL UNIVERSITE LIBRE DE BRUXELLES BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
 * EVEN IF UNIVERSITE LIBRE DE BRUXELLES HAS BEEN ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * UNIVERSITE LIBRE DE BRUXELLES SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON
 * AN "AS IS" BASIS, AND UNIVERSITE LIBRE DE BRUXELLES HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS. 
 *
 *****************************************************************************/

/**
 * @brief A program that uses the MEOS library for a demonstration of
 * the performance of the library.
 * We include a benchmark of the library with 12 queries.
 * The program can be build as follows
 * @code
 * gcc -Wall -g -I/usr/local/include  -o meos_benchmark meos_benchmark.c -L/usr/local/lib -lmeos 
 * @endcode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <float.h>

/* MEOS */
#include <meos.h>
#include <meos_internal.h>
#include <time.h>

/* Number of instants to send in batch to the file  */
#define NO_INSTANTS_BATCH 500
/* Number of instants to keep when restarting a sequence */
#define NO_INSTANTS_KEEP 2
/* Maximum length in characters of a header record in the input CSV file */
#define MAX_LENGTH_HEADER 1024
/* Maximum length in characters of a point in the input data */
#define MAX_LENGTH_POINT 64
/* Maximum length for a SQL output query */
#define MAX_LENGTH_SQL 16384
/* Number of inserts that are sent in bulk */
#define NO_BULK_INSERT 20
/* Maximum number of trips */
#define MAX_TRIPS 5614
/* Maximum number of ships */
#define MAX_SHIPS 100000
/* Maximum number of ports */
#define MAX_PORTS 3


int count=0;

typedef struct
{
  const GSERIALIZED *geom;
} Port_record;


typedef struct
{
  Timestamp T;
  long int MMSI;
  double Latitude;
  double Longitude;
  double SOG;
} AIS_record;

typedef struct
{
  long int MMSI;   /* Identifier of the trip */
  TSequence *trip; /* Array of Latest observations of the trip, by MMSI */
} trip_record;

//verifies if the size if greater than the max size, of so, it sends the data to the file
int windowManager(int size, trip_record *trips, int ship ,FILE *fileOut)
{
  if (trips[ship].trip && trips[ship].trip->count == NO_INSTANTS_BATCH)
  {

    char *temp_out = tsequence_out(trips[ship].trip, 15);
    fprintf(fileOut, "%ld, %s\n",trips[ship].MMSI, temp_out);
    printf("write in file\n");
    /* Free memory */
    free(temp_out);
    count++;
    printf("*");
    fflush(stdout);
    /* Restart the sequence by only keeping the last instants */
    tsequence_restart(trips[ship].trip, NO_INSTANTS_KEEP);

  }
  return 1;
}


int
main(int argc, char **argv)
{
  AIS_record rec;
  Port_record portRead;
  int no_records = 0;
  int no_nulls = 0;
  int no_ports = 2;
  int no_shipsBB = 0;
  int no_ferries = 0;
  int no_trips = 0;
  int no_beltships = 0;
  const Interval *maxt = pg_interval_in("1 day", -1);
  char point_buffer[MAX_LENGTH_POINT];
  char text_buffer[MAX_LENGTH_HEADER];
  /* Allocate space to build the allships */
  trip_record allships[MAX_TRIPS] = {0};
  trip_record ships[MAX_SHIPS] = {0};
  trip_record ferries[MAX_SHIPS] = {0};
  trip_record ferriesTrips[MAX_SHIPS] = {0};
  trip_record beltships[MAX_SHIPS] = {0};
  /* Allocate space to build the ports */
  Port_record ports[MAX_PORTS] = {0};
  /* Number of ships */
  int numships = 0;
  /* Iterator variable */
  int i;
  /* Return value */
  int return_value = 0;

  /***************************************************************************
   * Section 1: Open the output file
   ***************************************************************************/

  /* Open/create the output file */
  /* You may substitute the full file path in the first argument of fopen */
  FILE *fileOut = fopen("aisout.csv", "w+");
  printf("file out opened\n");

  if (! fileOut)
  {
    printf("Error creating/opening logfile\n");
    return_value = 1;
    goto cleanup;
  }


  /***************************************************************************
   * Section 2: Initialize MEOS, open the input AIS file
   ***************************************************************************/

  /* Initialize MEOS */
  meos_initialize(NULL);

  /* You may substitute the full file path in the first argument of fopen */
  FILE *fileIn = fopen("query2.csv", "r");
  printf("file in opened\n");

  if (! fileIn)
  {
    printf("Error opening input file\n");
    return_value = 1;
    goto cleanup;
  }

  /***************************************************************************
   * Section 3: Read input file line by line and append each observation as a
   * temporal point in MEOS
   * 
   ***************************************************************************/

  //printf("Accumulating %d instants before sending them to the logfile\n" "(one marker every logfile update)\n", NO_INSTANTS_BATCH);
  /* Read the first line of the file with the headers */
  fscanf(fileIn, "%1023s\n", text_buffer);

  /* Continue reading the file */
  do
  {
    printf("read %d\n",no_records);
    int read = fscanf(fileIn, "%32[^,],%ld,%lf,%lf,%lf\n",
      text_buffer, &rec.MMSI, &rec.Latitude, &rec.Longitude, &rec.SOG);
    /* Transform the string representing the timestamp into a timestamp value */
    rec.T = pg_timestamp_in(text_buffer, -1);

    if (read == 5)
      no_records++;

    if (read != 5 && ! feof(fileIn))
    {
      printf("Record with missing values ignored\n");
      no_nulls++;
    }

    if (ferror(fileIn))
    {
      printf("Error reading file\n");
      fclose(fileIn);
      fclose(fileOut);
    }

    /* Find the place to store the new instant */
    int ship = -1;
    for (i = 0; i < MAX_TRIPS; i++)
    {
      if (allships[i].MMSI == rec.MMSI)
      {
        ship = i;
        break;
      }
    }
    if (ship < 0)
    {
      ship = numships++;
      if (ship == MAX_TRIPS)
      {
        printf("The maximum number of ships in the input file is bigger than %d",MAX_TRIPS);
        return_value = 1;
        goto cleanup;
      }
      allships[ship].MMSI = rec.MMSI;
    }
    /*
     * Append the latest observation to the corresponding ship.
     * In the input file it is assumed that
     * - The coordinates are given in the WGS84 geographic coordinate system
     * - The timestamps are given in GMT time zone
     */
    char *t_out = pg_timestamp_out(rec.T);
    sprintf(point_buffer, "SRID=25832;Point(%lf %lf)@%s+00", rec.Longitude,
      rec.Latitude, t_out);

    //windowManager(NO_INSTANTS_BATCH,ships, ship,fileOut);

    /* Append the last observation */
    TInstant *inst = (TInstant *) tgeompoint_in(point_buffer);
    if (! allships[ship].trip)
      allships[ship].trip = tsequence_make_exp((const TInstant **) &inst, 1,
        NO_INSTANTS_BATCH, true, true, LINEAR, false);
    else
      tsequence_append_tinstant(allships[ship].trip , inst, 1000, maxt,true);
  } while (!feof(fileIn));

  printf("\n%d records read.\n%d incomplete records ignored. %d writes to the logfile\n",
    no_records, no_nulls,count);

    /***************************************************************************
   * Section 4: Create ports geometry
   ***************************************************************************/

    char *polygon_wkt_Rodby = "SRID=25832;Polygon((651135 6058230,651422 6058230,651422 6058517,651135 6058517,651135 6058230))";
    ports[0].geom = gserialized_in(polygon_wkt_Rodby, -1);
    printf("\n Created Rodby\n");
    
    char *polygon_wkt_Puttgarden = "SRID=25832;Polygon((644339 6042108,644896 6042487,644896 6042487,644339 6042108,644339 6042108))";
    ports[1].geom = gserialized_in(polygon_wkt_Puttgarden, -1);
    printf("\n Created Rodby\n");


   /***************************************************************************
   * Section 5: Create bounding box that incorates both ports
   ****************************************************************************/
    char *polygon_wkt_BoundingBox = "SRID=25832;Polygon((644339 6042108, 651422 6058548, 651422 6058548, 644339 6042108, 644339 6042108))";
    ports[2].geom = gserialized_in(polygon_wkt_BoundingBox, -1);

    printf("\n Created Bounding box\n");

    /* separate allships that are near the bounding box */
    for (size_t i = 0; i < numships; i++)
    {
      if (eintersects_tpoint_geo((const Temporal *) allships[i].trip, ports[2].geom))
      {
        printf("\n Ship is in the bounding box\n");
        ships[no_shipsBB++].MMSI = allships[i].MMSI;
        ships[no_shipsBB++].trip = allships[i].trip;
      }
      else
      {
        printf("\n Ship %d is not in the bounding box %d\n", allships[i].MMSI, allships[i].trip->count);
      }
    }


  /***************************************************************************
   * Section 6: Separate Ferries from all ships 
   ****************************************************************************/
    /* separate allships that are near the ports */
    for (size_t i = 0; i < no_shipsBB; i++)
    {
      if (eintersects_tpoint_geo((const Temporal *) ships[i].trip, ports[0].geom) 
      && (eintersects_tpoint_geo((const Temporal *) ships[i].trip, ports[1].geom)))
      {    
        printf("\n Ship %d is in Rodby and Puttergarten\n", ships[i].MMSI);
        ferries[no_ferries++].MMSI = ships[i].MMSI;
        ferries[no_ferries++].trip = ships[i].trip;
      }
      else 
      {
        printf("\n Ship %d is not in Rodby and Puttergarten\n", ships[i].MMSI);
      }
    } 



   /***************************************************************************
   * Section 6: Split Ferries trips into trips between ports - AtGeometry
   ****************************************************************************/

    /* Separate trips that are near the ports atSTbox */
    for (int i = 0; i < no_ferries; i++)
    {
      for (int j = 0; j < no_ports; j++)
      {
        Temporal *atgeom = tpoint_at_geometry((const Temporal *)ferries[i].trip, ports[j].geom);
        if (atgeom)
        {
          printf("\n Ship %d is in port %d\n", ferries[i].MMSI, j);
          ferriesTrips[no_trips++].MMSI = ferries[i].MMSI;
          ferriesTrips[no_trips++].trip = ferries[i].trip;
        }
        else
        {
          printf("\n Ship %d is not in port %d\n", allships[i].MMSI, j);
        }
        free(atgeom);
      }
    }

   /***************************************************************************
   * Section 7: Split Ferries trips into trips between ports - AtStops
   ****************************************************************************/
     /* Separate trips that are near the ports atSTbox */
    for (int i = 0; i < no_ferries; i++)
    {
      for (int j = 0; j < no_ports; j++)
      {
        TSequenceSet * atstops =  temporal_stops((const Temporal *)ferries[i].trip,25, 0);
        if (atstops)
        {
          printf("\n Ship %d is in port %d\n", ferries[i].MMSI, j);
          ferriesTrips[no_trips++].MMSI = ferries[i].MMSI;
          ferriesTrips[no_trips++].trip = ferries[i].trip;
        }
        else
        {
          printf("\n Ship %d is not in port %d\n", allships[i].MMSI, j);
        }
      }
    }
  

   /***************************************************************************
   * Section 8 : Trips Functions
   ****************************************************************************/

    /* Total distance */
    double totalDistance = 0;
    double dist;
    for (int i = 0; i < numships; i++)
    {
      dist = tpoint_length((const Temporal *)allships[i].trip) / 1000;
      printf("\n Ship %d has a distance of %lf\n", allships[i].MMSI, dist);
      totalDistance += dist;
    }
    printf("\n Total Distance is %lf\n", totalDistance);

    /* Average Speed */
    double totalSpeed = 0;
    for (int i = 0; i < numships; i++)
    {
      double speed = average_speed(allships[i].trip);
      printf("\n Ship %d has a speed of %lf\n", allships[i].MMSI, speed);
      totalSpeed += speed;
    }
    printf("\n Average Speed is %lf\n", totalSpeed/no_trips);

   /***************************************************************************
   * Section 9: ships that get close to ferries
   ****************************************************************************/
     /* separate allships that are not ferries */
    for (size_t i = 0; i < numships; i++)
    {
      if (eintersects_tpoint_geo((const Temporal *) allships[i].trip, ports[0].geom) 
      && (eintersects_tpoint_geo((const Temporal *) allships[i].trip, ports[1].geom)))
      {    
        printf("\n Ship %d is a ferry\n", allships[i].MMSI);
      }
      else 
      {
        printf("\n Ship %d is not in Rodby and Puttergarten\n", allships[i].MMSI);
        beltships[no_beltships++].MMSI = allships[i].MMSI;
        beltships[no_beltships++].trip = allships[i].trip;
      }
    } 

    /* Find ships that are near ferries within 300 meters */
    for (size_t i = 0; i < numships; i++)
    {
      for (size_t i = 0; i < numships; i++)
      {
        if (allships[i].MMSI != allships[i].MMSI)
        {
          printf("\n Ship is not the same\n");
          temporal * aux = tdwithin_tpoint_tpoint((const Temporal *)allships[i].trip, (const Temporal *)allships[j].trip,5.5,true,false);
          if (aux)
          {
            free(aux);
          }
        }
      }
    }


  
    /***************************************************************************/

/* Clean up */
cleanup:
 /* Free memory */
  for (i = 0; i < numships; i++)
    free(allships[i].trip);

  /* Finalize MEOS */
  meos_finalize();

  /* Close the connection to the logfile */
  fclose(fileOut);
  /* Close the file */
  fclose(fileIn);

  return return_value;
}
