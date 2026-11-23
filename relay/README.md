# Relay server

Pairs a host and a friend by their join code and passes the traffic between them, so neither needs to open ports on
a router. Deploy it once (free) on Render: New > Web Service > this repository, Root Directory `relay`,
Build `npm install`, Start `node server.js`, Instance type Free. Put the URL in `mods-data/relay.txt`.
