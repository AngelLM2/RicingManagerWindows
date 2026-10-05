#include "StringUtils.hpp"

#include <string>
#include <cctype>


std::string StringUtils::toLower(std::string text)
    {
        for(char& c : text)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }   
    
        return text;
        
    };