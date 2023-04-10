SELECT round (angularDifference3points ( tgeompoint '[Point (1 1) @2000-01-01, Point (2 2) @2000-01-02, Point (1 1) @2000-01-03]'), 3);

SELECT round (angularDifference3points ( tgeompoint '[Point (1 1) @2000-01-01, Point (2 2) @2000-01-02, Point (1 1) @2000-01-03,Point (4 4) @2000-01-04]'), 4);

SELECT round (angularDifference3points ( tgeompoint '[Point (1 1) @2000-01-01, Point (2 2) @2000-01-02, Point (1 1) @2000-01-03,Point (4 4) @2000-01-04, Point (1 1) @2000-01-05]'), 5);