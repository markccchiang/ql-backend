/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*! \file capabilities.hpp
    \brief what this build prices, as data
*/

#ifndef qlservice_session_capabilities_hpp
#define qlservice_session_capabilities_hpp

namespace quantlib::v2 {
    class Capabilities;
}

namespace qlservice {

    //! Fills the reply to a Hello.
    /*! The lists here and the dispatch in session.cpp are the same fact told
        twice, and the second telling is the one that goes over the wire. They
        have to move together: a style added to Session::priceOption and not
        added here is a style no client will offer, and one added here and not
        there is a client authoring a request that will be refused.

        That is still better than the alternative it replaces, which was every
        client keeping its own copy of HANDLERS.md and discovering the drift
        when a user hit it. The two copies are now in one repository, in
        neighbouring files, and test/smoke_v2.py checks that what is advertised
        can actually be priced.
    */
    void fillCapabilities(quantlib::v2::Capabilities& out);

}

#endif
