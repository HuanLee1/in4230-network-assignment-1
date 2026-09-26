#!/usr/bin/env python

from mininet.topo import Topo

# Usage example:
# sudo mn --custom oblig-topology.py --topo oblig --link tc --xterms

class ObligTopo( Topo ):
    "Simple topology for mandatory assignment."

    def __init__( self ):
        "Set up our custom topo."

        # Initialize topology
        Topo.__init__( self )

        # Add hosts
        A = self.addHost('A')
        B = self.addHost('B')
        C = self.addHost('C')

        # Add links
        self.addLink(A, B, bw=10, delay='10ms', loss=0.0, use_tbf=False)
        self.addLink(B, C, bw=10, delay='10ms', loss=0.0, use_tbf=False)


topos = { 'oblig': ( lambda: ObligTopo() ) }