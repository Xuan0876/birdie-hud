Copernicus DEM licence (GLO-30), as published by the licensor.

License-COPDEM-30.pdf  "Licence for Copernicus DEM instance COP-DEM-GLO-30-F Global 30m Full, Free & Open"
  from https://docs.sentinel-hub.com/api/latest/static/files/data/dem/resources/license/License-COPDEM-30.pdf
  (linked from the dataset's licence page,
  https://dataspace.copernicus.eu/explore-data/data-collections/copernicus-contributing-missions/collections-description/COP-DEM),
  fetched 2026-11-22. GLO-90 is released under the same terms (Copernicus DEM readme on the AWS bucket).

Birdie ships no elevation data. It downloads tiles on the user's machine from the AWS open data buckets
copernicus-dem-30m / copernicus-dem-90m (or imports tiles the user downloaded), converts them to its own
.bdem format and draws shading and a skyline from them. Article 6 of the licence applies to that use; see
THIRD_PARTY_NOTICES.md for the notices Birdie gives.
